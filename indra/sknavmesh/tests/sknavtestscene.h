/**
 * @file sknavtestscene.h
 * @brief Synthetic scenes of boxes over ground, sampled into tiles of air, for the navmesh tests.
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

#ifndef SK_NAVTESTSCENE_H
#define SK_NAVTESTSCENE_H

#include "../sknavbuild.h"
#include "../sknavenclosure.h"
#include "../sknavspaces.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace sknav_test
{
    using namespace sknav;

    const F32 CELL = 0.125f;
    const F32 UNIT = 0.125f;
    const S32 SIDE = 256;           // cells per 32 m tile
    const F32 TOP = 40.f;

    struct Solid
    {
        F32 mX0, mY0, mX1, mY1, mZ0, mZ1;
        bool mWalk;
        bool mFlight;
    };

    /// Axis-aligned solids over ground, sampled at cell centres into tiles.
    struct Scene
    {
        std::vector<Solid> mSolids;
        F32 mGround[4] = { -1e9f, -1e9f, 1e9f, 1e9f };     ///< where the ground is: x0, y0, x1, y1
        F32 mShift = 0.f;                                   ///< every solid moved this far along x

        void box(F32 x0, F32 y0, F32 x1, F32 y1, F32 z0, F32 z1, bool walk = true, bool flight = false)
        {
            mSolids.push_back({ x0, y0, x1, y1, z0, z1, walk, flight });
        }

        void wall(F32 x0, F32 y0, F32 x1, F32 y1, F32 z1 = 3.f) { box(x0, y0, x1, y1, 0.f, z1, false); }

        /// A flight up +x from x0, treads 0.25 m high, onto z_top.
        void flight(F32 x0, F32 x1, F32 y0, F32 y1, F32 z_top) { flight(x0, x1, y0, y1, 0.f, z_top); }

        /// A flight from z0 at x0 to z1 at x1, either way along x.
        void flight(F32 x0, F32 x1, F32 y0, F32 y1, F32 z0, F32 z1)
        {
            const S32 n = static_cast<S32>(std::lround((z1 - z0) / 0.25f));
            const F32 going = (x1 - x0) / n;
            for (S32 k = 1; k <= n; ++k)
            {
                const F32 a = x0 + (k - 1) * going, b = x0 + k * going;
                box(std::min(a, b), y0, std::max(a, b), y1, 0.f, z0 + 0.25f * k, true, true);
            }
        }

        TileAirPtr tile(S32 kx, S32 ky) const
        {
            AirGrid g;
            g.mOrigin.set(kx * 32.f, ky * 32.f, 0.f);
            g.mCell = CELL;
            g.mUnit = UNIT;
            g.mWidth = g.mHeight = SIDE;
            g.mStart.assign(SIDE * SIDE + 1, 0);
            struct Run
            {
                S32 mZ0, mZ1;
                bool mWalk, mFlight, mTerrain;
            };
            std::vector<Run> runs;
            for (S32 iy = 0; iy < SIDE; ++iy)
            {
                for (S32 ix = 0; ix < SIDE; ++ix)
                {
                    const F32 x = g.mOrigin.mV[VX] + (ix + 0.5f) * CELL - mShift, y = g.mOrigin.mV[VY] + (iy + 0.5f) * CELL;
                    runs.clear();
                    if (x >= mGround[0] && y >= mGround[1] && x < mGround[2] && y < mGround[3])
                    {
                        runs.push_back({ -8, 0, true, false, true });
                    }
                    for (const Solid& s : mSolids)
                    {
                        if (x >= s.mX0 && x < s.mX1 && y >= s.mY0 && y < s.mY1)
                        {
                            runs.push_back({ static_cast<S32>(std::lround(s.mZ0 / UNIT)), static_cast<S32>(std::lround(s.mZ1 / UNIT)), s.mWalk, s.mFlight, false });
                        }
                    }
                    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.mZ0 < b.mZ0; });
                    std::vector<Run> merged;
                    for (const Run& r : runs)
                    {
                        if (!merged.empty() && r.mZ0 <= merged.back().mZ1)
                        {
                            if (r.mZ1 >= merged.back().mZ1)
                            {
                                merged.back().mWalk = r.mWalk;
                                merged.back().mFlight = r.mFlight;
                                merged.back().mTerrain = r.mTerrain;
                            }
                            merged.back().mZ1 = std::max(merged.back().mZ1, r.mZ1);
                        }
                        else
                        {
                            merged.push_back(r);
                        }
                    }
                    for (size_t i = 0; i < merged.size(); ++i)
                    {
                        AirGap gap;
                        gap.mFloor = static_cast<U16>(merged[i].mZ1 + 8);   // heights from -1 m
                        gap.mFlags = merged[i].mWalk ? AirGap::WALKABLE : 0;
                        if (merged[i].mTerrain)
                        {
                            gap.mFlags |= static_cast<U8>(AREA_GROUND << AirGap::AREA_SHIFT);
                        }
                        if (merged[i].mFlight && merged[i].mWalk)
                        {
                            gap.mFlags |= AirGap::FLIGHT;
                        }
                        if (i + 1 < merged.size())
                        {
                            gap.mCeiling = static_cast<U16>(merged[i + 1].mZ0 + 8);
                        }
                        else
                        {
                            gap.mCeiling = static_cast<U16>(TOP / UNIT);
                            gap.mFlags |= AirGap::OPEN | AirGap::OWN_SKY;
                        }
                        g.mGaps.push_back(gap);
                    }
                    g.mStart[ix + iy * SIDE + 1] = static_cast<U32>(g.mGaps.size());
                }
            }
            g.mOrigin.mV[VZ] = -1.f;
            computeClearance(g, 6, 4);
            auto air = std::make_shared<TileAir>();
            distilAir(g, 6, *air);
            airSeams(g, *air);
            return air;
        }

        /// Every tile's solids published to a sky map in 1 m columns, as the viewer does.
        void addSky(EnclosureInput& in) const
        {
            auto sky = std::make_shared<SkyMap>();
            for (size_t i = 0; i < in.mTiles.size(); ++i)
            {
                const EnclosureInput::Tile& t = in.mTiles[i];
                TileAir tops;
                tops.mCell = CELL;
                tops.mUnit = UNIT;
                tops.mTopCell = static_cast<S32>(1.f / CELL);
                tops.mTopWidth = tops.mTopHeight = 32;
                tops.mTopStart.assign(32 * 32 + 1, 0);
                for (S32 ty = 0; ty < 32; ++ty)
                {
                    for (S32 tx = 0; tx < 32; ++tx)
                    {
                        // Any solid touching the metre counts, as the viewer's tops take every span under it.
                        const F32 x = t.mKey.mX * 32.f + tx, y = t.mKey.mY * 32.f + ty;
                        std::vector<TopInterval> runs;
                        if (x + 1.f > mGround[0] && y + 1.f > mGround[1] && x < mGround[2] && y < mGround[3])
                        {
                            runs.push_back({ 0, static_cast<U16>(1.f / UNIT) });
                        }
                        for (const Solid& sd : mSolids)
                        {
                            if (x + 1.f > sd.mX0 + mShift && x < sd.mX1 + mShift && y + 1.f > sd.mY0 && y < sd.mY1)
                            {
                                runs.push_back({ static_cast<U16>((sd.mZ0 + 1.f) / UNIT), static_cast<U16>((sd.mZ1 + 1.f) / UNIT) });
                            }
                        }
                        tops.mTop.insert(tops.mTop.end(), runs.begin(), runs.end());
                        tops.mTopStart[tx + ty * 32 + 1] = static_cast<U32>(tops.mTop.size());
                    }
                }
                sky->set(i + 1, LLVector3d(t.mKey.mX * 32.0, t.mKey.mY * 32.0, -1.0), tops);
            }
            in.mSky = sky;
        }

        std::shared_ptr<EnclosureInput> input(S32 tiles_x, S32 tiles_y = 1) const
        {
            auto in = std::make_shared<EnclosureInput>();
            for (S32 ky = 0; ky < tiles_y; ++ky)
            {
                for (S32 kx = 0; kx < tiles_x; ++kx)
                {
                    EnclosureInput::Tile t;
                    t.mKey.mX = kx;
                    t.mKey.mY = ky;
                    t.mAir = tile(kx, ky);
                    if (t.mAir && !t.mAir->mLeaves.empty())
                    {
                        in->mTiles.push_back(t);
                    }
                }
            }
            return in;
        }
    };

    /// Two 8 x 8 m rooms sharing a wall at x = 10, roofed, with a door out of the west one.
    Scene twoRooms()
    {
        Scene s;
        s.wall(2.f, 2.f, 18.f, 2.25f);
        s.wall(2.f, 9.75f, 18.f, 10.f);
        s.wall(17.75f, 2.f, 18.f, 10.f);
        s.wall(2.f, 2.f, 2.25f, 5.f);
        s.wall(2.f, 6.f, 2.25f, 10.f);
        s.box(2.f, 5.f, 2.25f, 6.f, 2.2f, 3.f, false);
        s.box(2.f, 2.f, 18.f, 10.f, 3.f, 3.25f);
        return s;
    }

    /// A mesh's air as a tool beside the navmesh exports writes it: tiles'
    /// level-0 air, doors, lift boxes, and points to answer at. The export
    /// carries no own-sky bit: own_sky gives it to every open gap, as
    /// extraction does.
    inline bool readReplay(const std::string& path, std::shared_ptr<EnclosureInput>& enc_in, std::shared_ptr<RoomsInput>& rooms,
                           std::vector<LLVector3d>& points, bool own_sky = false)
    {
        std::ifstream in(path, std::ios::binary);
        auto get = [&in](auto& v) { in.read(reinterpret_cast<char*>(&v), sizeof(v)); };
        char magic[4];
        U32 version;
        in.read(magic, 4);
        get(version);
        if (!in || std::string(magic, 4) != "SKRI" || version != 1)
        {
            return false;
        }
        LLVector3d origin;
        get(origin.mdV[0]);
        get(origin.mdV[1]);
        get(origin.mdV[2]);
        F32 head;
        get(head);
        U32 tiles;
        get(tiles);
        enc_in = std::make_shared<EnclosureInput>();
        enc_in->mMeshOrigin = origin;
        enc_in->mParams.mWalkHeadM = head;
        for (U32 t = 0; t < tiles; ++t)
        {
            EnclosureInput::Tile tile;
            AirGrid g;
            U32 gaps;
            get(tile.mKey.mX);
            get(tile.mKey.mY);
            get(g.mOrigin.mV[0]);
            get(g.mOrigin.mV[1]);
            get(g.mOrigin.mV[2]);
            get(g.mCell);
            get(g.mUnit);
            get(g.mWidth);
            get(g.mHeight);
            get(gaps);
            g.mStart.assign(static_cast<size_t>(g.mWidth) * g.mHeight + 1, 0);
            for (S32 i = 0; i < g.mWidth * g.mHeight; ++i)
            {
                U32 n;
                get(n);
                g.mStart[i + 1] = g.mStart[i] + n;
            }
            g.mGaps.resize(gaps);
            for (AirGap& gap : g.mGaps)
            {
                get(gap.mFloor);
                get(gap.mCeiling);
                get(gap.mFlags);
                if (own_sky && (gap.mFlags & AirGap::OPEN))
                {
                    gap.mFlags |= AirGap::OWN_SKY;
                }
            }
            const S32 climb = static_cast<S32>(std::floor(0.75f / g.mUnit));
            computeClearance(g, climb, static_cast<S32>(std::ceil(0.5f / g.mUnit)));
            auto air = std::make_shared<TileAir>();
            distilAir(g, climb, *air);
            airSeams(g, *air);
            tile.mAir = air;
            if (!air->mLeaves.empty())
            {
                enc_in->mTiles.push_back(tile);
            }
        }
        rooms = std::make_shared<RoomsInput>();
        rooms->mMeshOrigin = origin;
        rooms->mParams.mWalkHeadM = head;
        U32 doors;
        get(doors);
        for (U32 i = 0; i < doors; ++i)
        {
            DoorEntity d;
            U8 known;
            get(d.mPlanePoint.mdV[0]);
            get(d.mPlanePoint.mdV[1]);
            get(d.mPlanePoint.mdV[2]);
            get(d.mNormal.mV[0]);
            get(d.mNormal.mV[1]);
            get(d.mNormal.mV[2]);
            get(d.mWidth);
            get(d.mHeight);
            get(d.mSill);
            get(known);
            d.mPlaneKnown = known != 0;
            for (S32 k = 0; k < 3; ++k)
            {
                get(d.mMin.mdV[k]);
            }
            for (S32 k = 0; k < 3; ++k)
            {
                get(d.mMax.mdV[k]);
            }
            d.mId.generate();
            rooms->mDoors.push_back(d);
        }
        U32 lifts;
        get(lifts);
        for (U32 i = 0; i < lifts; ++i)
        {
            LLVector3d lo, hi;
            for (S32 k = 0; k < 3; ++k)
            {
                get(lo.mdV[k]);
            }
            for (S32 k = 0; k < 3; ++k)
            {
                get(hi.mdV[k]);
            }
            rooms->mLifts.emplace_back(lo, hi);
        }
        U32 queries;
        get(queries);
        points.resize(queries);
        for (LLVector3d& q : points)
        {
            get(q.mdV[0]);
            get(q.mdV[1]);
            get(q.mdV[2]);
        }
        return static_cast<bool>(in);
    }
}

#endif // SK_NAVTESTSCENE_H
