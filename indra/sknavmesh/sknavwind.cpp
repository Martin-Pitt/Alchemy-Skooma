/**
 * @file sknavwind.cpp
 * @brief Wind over a mesh's air: a speed factor and a deflection per cell, per heading and height band.
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

#include "linden_common.h"

#include "sknavwind.h"

#include "sknavbuild.h"
#include "sknavprofile.h"

#include "llmath.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <iterator>
#include <limits>

namespace sknav
{
    namespace
    {
        constexpr F32 SOLID_COLUMN_M = 100.f;   ///< how tall a column with no air at all reads
        constexpr F32 SWEEP_M = 64.f;           ///< no lee is looked for further upwind
        constexpr F32 CHAMFER_SLACK = 1.0824f;  ///< the 8-neighbour chamfer overestimates by at most this
        constexpr F32 SQRT2 = 1.41421356f;

        F32 unitsToM(const TileEnclosure& t, S32 units) { return t.mOrigin.mV[VZ] + units * t.mUnit; }

        /// The 8-neighbour chamfer from blocked cells, scaled down to a lower bound on the true distance.
        void reachField(WindField& f)
        {
            SK_NAV_ZONE("nav.wind.reach");
            const S32 w = f.mWidth, h = f.mHeight;
            const F32 inf = static_cast<F32>(w + h) * 2.f;
            f.mReach.assign(static_cast<size_t>(w) * h, inf);
            for (size_t i = 0; i < f.mReach.size(); ++i)
            {
                if (f.mFlags[i] & WindCell::BLOCKED)
                {
                    f.mReach[i] = 0.f;
                }
            }
            auto relax = [&](S32 x, S32 y, S32 dx, S32 dy, F32 cost) {
                const S32 nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < w && ny < h)
                {
                    F32& d = f.mReach[x + y * w];
                    d = std::min(d, f.mReach[nx + ny * w] + cost);
                }
            };
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    relax(x, y, -1, 0, 1.f);
                    relax(x, y, 0, -1, 1.f);
                    relax(x, y, -1, -1, SQRT2);
                    relax(x, y, 1, -1, SQRT2);
                }
            }
            for (S32 y = h - 1; y >= 0; --y)
            {
                for (S32 x = w - 1; x >= 0; --x)
                {
                    relax(x, y, 1, 0, 1.f);
                    relax(x, y, 0, 1, 1.f);
                    relax(x, y, 1, 1, SQRT2);
                    relax(x, y, -1, 1, SQRT2);
                }
            }
            for (F32& d : f.mReach)
            {
                d /= CHAMFER_SLACK;
            }
        }

        /// Cells a march may skip from a cell this far from any solid, and still land on the first one.
        S32 skip(F32 reach) { return std::max(1, static_cast<S32>(reach - 0.75f)); }

        /// A normalised Gaussian kernel of radius three sigma.
        std::vector<F32> gaussian(F32 sigma_cells)
        {
            const S32 r = static_cast<S32>(3.f * sigma_cells);
            std::vector<F32> k(2 * r + 1);
            F32 sum = 0.f;
            for (S32 i = -r; i <= r; ++i)
            {
                k[i + r] = std::exp(-0.5f * (i / sigma_cells) * (i / sigma_cells));
                sum += k[i + r];
            }
            for (F32& x : k)
            {
                x /= sum;
            }
            return k;
        }

        /// A separable blur of v by k, edges repeating: along x, then along y. Each tap is added to a whole row in
        /// kernel order, which streams along memory and keeps every cell's sum in that order.
        void blur(const std::vector<F32>& v, const std::vector<F32>& k, S32 w, S32 h, std::vector<F32>& out)
        {
            SK_NAV_ZONE("nav.wind.blur");
            const S32 r = static_cast<S32>(k.size() / 2);
            std::vector<F32> tmp(v.size()), row(static_cast<size_t>(w) + 2 * r);
            for (S32 y = 0; y < h; ++y)
            {
                const F32* src = v.data() + static_cast<size_t>(y) * w;
                for (S32 x = -r; x < w + r; ++x)
                {
                    row[x + r] = src[std::clamp(x, 0, w - 1)];
                }
                F32* dst = tmp.data() + static_cast<size_t>(y) * w;
                for (S32 i = 0; i <= 2 * r; ++i)
                {
                    const F32 ki = k[i];
                    const F32* tap = row.data() + i;
                    for (S32 x = 0; x < w; ++x)
                    {
                        dst[x] += ki * tap[x];
                    }
                }
            }
            out.assign(v.size(), 0.f);
            for (S32 y = 0; y < h; ++y)
            {
                F32* dst = out.data() + static_cast<size_t>(y) * w;
                for (S32 i = 0; i <= 2 * r; ++i)
                {
                    const F32 ki = k[i];
                    const F32* tap = tmp.data() + static_cast<size_t>(std::clamp(y + i - r, 0, h - 1)) * w;
                    for (S32 x = 0; x < w; ++x)
                    {
                        dst[x] += ki * tap[x];
                    }
                }
            }
        }

        /// A separable Gaussian over the valid cells only, normalised by their smoothed weight; edges repeat.
        void smooth(std::vector<F32>& v, const std::vector<F32>& weight, const std::vector<F32>& weight_blur, S32 w, S32 h, F32 sigma_cells)
        {
            SK_NAV_ZONE("nav.wind.smooth");
            std::vector<F32> num(v.size());
            for (size_t i = 0; i < v.size(); ++i)
            {
                num[i] = v[i] * weight[i];
            }
            std::vector<F32> sum;
            blur(num, gaussian(sigma_cells), w, h, sum);
            for (size_t i = 0; i < v.size(); ++i)
            {
                v[i] = sum[i] / std::max(weight_blur[i], 1e-6f);
            }
        }

        /// One for an open cell, nothing for a blocked one.
        std::vector<F32> windWeight(const WindField& f)
        {
            std::vector<F32> weight(f.mFlags.size(), 0.f);
            for (size_t i = 0; i < weight.size(); ++i)
            {
                weight[i] = (f.mFlags[i] & WindCell::BLOCKED) ? 0.f : 1.f;
            }
            return weight;
        }
    }

    F32 WindCell::factor() const
    {
        return mFactor * WIND_FACTOR_UNIT;
    }

    LLVector2 WindCell::deflection() const
    {
        return LLVector2(mDeflect[0] * WIND_DEFLECT_UNIT, mDeflect[1] * WIND_DEFLECT_UNIT);
    }

    const WindCell* WindSlab::at(F32 x, F32 y) const
    {
        const S32 ix = static_cast<S32>(std::floor((x - mOrigin.mV[VX]) / mCell));
        const S32 iy = static_cast<S32>(std::floor((y - mOrigin.mV[VY]) / mCell));
        if (ix < 0 || iy < 0 || ix >= mWidth || iy >= mHeight)
        {
            return nullptr;
        }
        return &mCells[ix + iy * mWidth];
    }

    size_t WindSlab::bytes() const
    {
        return sizeof(WindSlab) + mCells.capacity() * sizeof(WindCell);
    }

    WindSlabPtr WindProduct::slab(S32 heading, S32 band) const
    {
        heading = ((heading % WIND_HEADINGS) + WIND_HEADINGS) % WIND_HEADINGS;
        const size_t i = static_cast<size_t>(band) * WIND_HEADINGS + heading;
        return band >= 0 && i < mSlabs.size() ? mSlabs[i] : nullptr;
    }

    size_t WindProduct::bytes() const
    {
        size_t n = sizeof(WindProduct);
        for (size_t i = 0; i < mSlabs.size(); ++i)
        {
            if (mSlabs[i])
            {
                n += mSlabs[i]->bytes();
                // The band's heights, counted once.
                n += i % WIND_HEADINGS == 0 && mSlabs[i]->mZ ? mSlabs[i]->mZ->capacity() * sizeof(F32) : 0;
            }
        }
        return n;
    }

    WindFieldPtr buildWindField(const EnclosureProduct& enclosure, const RoomsProduct* rooms, const WindLevel& level, const WindParams& params)
    {
        SK_NAV_ZONE("nav.wind.field");
        auto f = std::make_shared<WindField>();
        f->mCell = params.mCellM;
        f->mLevel = level;
        F32 x0 = std::numeric_limits<F32>::max(), y0 = x0, x1 = -x0, y1 = -x0;
        for (const TileEnclosure& t : enclosure.mTiles)
        {
            if (t.mAir)
            {
                x0 = std::min(x0, t.mOrigin.mV[VX]);
                y0 = std::min(y0, t.mOrigin.mV[VY]);
                x1 = std::max(x1, t.mOrigin.mV[VX] + t.mWidth * t.mCell);
                y1 = std::max(y1, t.mOrigin.mV[VY] + t.mHeight * t.mCell);
            }
        }
        if (x1 <= x0 || y1 <= y0)
        {
            return f;
        }
        const F32 cell = f->mCell;
        f->mOrigin.set(x0, y0);
        f->mWidth = std::max(1, static_cast<S32>(std::ceil((x1 - x0) / cell - 0.01f)));
        f->mHeight = std::max(1, static_cast<S32>(std::ceil((y1 - y0) / cell - 0.01f)));
        const S32 w = f->mWidth, h = f->mHeight;
        const size_t n = static_cast<size_t>(w) * h;
        const F32 none = std::numeric_limits<F32>::max();

        // Every fine cell of a detailed tile, with its raster cell.
        auto forCells = [&](auto&& fn) {
            for (const TileEnclosure& t : enclosure.mTiles)
            {
                if (!t.mAir)
                {
                    continue;
                }
                const S32 bx = static_cast<S32>(std::lround((t.mOrigin.mV[VX] - x0) / t.mCell));
                const S32 by = static_cast<S32>(std::lround((t.mOrigin.mV[VY] - y0) / t.mCell));
                const F32 per = cell / t.mCell;
                for (S32 iy = 0; iy < t.mHeight; ++iy)
                {
                    const S32 ry = std::min(h - 1, static_cast<S32>((by + iy + 0.5f) / per));
                    for (S32 ix = 0; ix < t.mWidth; ++ix)
                    {
                        const S32 rx = std::min(w - 1, static_cast<S32>((bx + ix + 0.5f) / per));
                        fn(t, ix, iy, static_cast<size_t>(rx) + static_cast<size_t>(ry) * w);
                    }
                }
            }
        };
        // Each leaf's gaps over its cells, looked up once per tile.
        SK_NAV_ZONE("nav.wind.field.leaves");
        std::vector<std::vector<S32>> leafOf(enclosure.mTiles.size());
        for (size_t ti = 0; ti < enclosure.mTiles.size(); ++ti)
        {
            const TileEnclosure& t = enclosure.mTiles[ti];
            if (!t.mAir)
            {
                continue;
            }
            std::vector<S32>& lo = leafOf[ti];
            lo.assign(static_cast<size_t>(t.mWidth) * t.mHeight, -1);
            for (size_t li = 0; li < t.mAir->mLeaves.size(); ++li)
            {
                const AirLeaf& l = t.mAir->mLeaves[li];
                const S32 side = 1 << l.mLevel;
                for (S32 y = l.mY; y < std::min<S32>(l.mY + side, t.mHeight); ++y)
                {
                    for (S32 x = l.mX; x < std::min<S32>(l.mX + side, t.mWidth); ++x)
                    {
                        lo[x + y * t.mWidth] = static_cast<S32>(li);
                    }
                }
            }
        }
        // Leaves fill finest last, so the map holds each cell's finest leaf; the column gathers the rest.
        // Cells walk their leaves in rows, so a column is gathered again only when the leaf changes.
        const TileEnclosure* col_tile = nullptr;
        S32 col_leaf = -1;
        auto column = [&](const TileEnclosure& t, S32 ix, S32 iy, AirColumn& out) {
            const S32 leaf = leafOf[&t - enclosure.mTiles.data()][ix + iy * t.mWidth];
            if (&t != col_tile || leaf != col_leaf)
            {
                airColumn(*t.mAir, leaf, out);
                col_tile = &t;
                col_leaf = leaf;
            }
            return out.mCount > 0;
        };
        AirColumn col;

        // The ground: the lowest terrain floor, else carried from the nearest one.
        SK_NAV_ZONE("nav.wind.field.ground");
        f->mZ.assign(n, none);
        if (level.mAbsolute)
        {
            std::fill(f->mZ.begin(), f->mZ.end(), level.mZ);
        }
        else
        {
            std::vector<F32> ground(n, none);
            F32 lowest = none;
            forCells([&](const TileEnclosure& t, S32 ix, S32 iy, size_t r) {
                if (!column(t, ix, iy, col))
                {
                    return;
                }
                const AirGap& g = t.mAir->mGaps[col.mGap[0]];
                const F32 z = unitsToM(t, airFloorUnits(col.floorAt(*t.mAir, 0, ix, iy)));
                lowest = std::min(lowest, z);
                if (g.area() == AREA_GROUND)
                {
                    ground[r] = std::min(ground[r], z);
                }
            });
            std::deque<size_t> queue;
            for (size_t i = 0; i < n; ++i)
            {
                if (ground[i] != none)
                {
                    queue.push_back(i);
                }
            }
            if (queue.empty())
            {
                std::fill(ground.begin(), ground.end(), lowest == none ? 0.f : lowest);
            }
            while (!queue.empty())
            {
                const size_t i = queue.front();
                queue.pop_front();
                const S32 x = static_cast<S32>(i % w), y = static_cast<S32>(i / w);
                const S32 nb[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
                for (const auto& q : nb)
                {
                    if (q[0] >= 0 && q[1] >= 0 && q[0] < w && q[1] < h && ground[q[0] + q[1] * w] == none)
                    {
                        ground[q[0] + q[1] * w] = ground[i];
                        queue.push_back(q[0] + q[1] * w);
                    }
                }
            }
            for (size_t i = 0; i < n; ++i)
            {
                f->mZ[i] = ground[i] + level.mOffsetM;
            }
        }

        // The slab through every fine cell: in air, or in a solid of some height.
        SK_NAV_ZONE("nav.wind.field.slab");
        std::vector<U16> covered(n, 0), solid(n, 0);
        std::vector<const TileEnclosure*> repTile(n, nullptr);
        std::vector<U32> repGap(n, 0);
        f->mSolid.assign(n, 0.f);
        forCells([&](const TileEnclosure& t, S32 ix, S32 iy, size_t r) {
            ++covered[r];
            const F32 z = f->mZ[r];
            F32 above = none;
            if (column(t, ix, iy, col))
            {
                const F32 slack = t.mUnit;      // floors may read a unit high
                for (U32 k = 0; k < col.mCount; ++k)
                {
                    const U32 gi = col.mGap[k];
                    const AirGap& g = t.mAir->mGaps[gi];
                    const F32 lo = unitsToM(t, airFloorUnits(col.floorAt(*t.mAir, k, ix, iy)));
                    const bool open = g.mCeiling == AIR_CEILING_OPEN;
                    if (z >= lo - slack && (open || z < unitsToM(t, g.mCeiling)))
                    {
                        if (!repTile[r])
                        {
                            repTile[r] = &t;
                            repGap[r] = gi;
                        }
                        return;
                    }
                    if (lo > z)
                    {
                        above = std::min(above, lo);
                    }
                }
            }
            ++solid[r];
            f->mSolid[r] = std::max(f->mSolid[r], above == none ? SOLID_COLUMN_M : above - z);
        });

        f->mFlags.assign(n, 0);
        f->mSpace.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
        {
            U8& fl = f->mFlags[i];
            if (!covered[i])
            {
                fl = WindCell::UNSEEN;
                continue;
            }
            if (solid[i] >= params.mBlockedShare * covered[i] || !repTile[i])
            {
                fl = WindCell::BLOCKED;
                continue;
            }
            f->mSolid[i] = 0.f;
            const TileEnclosure& t = *repTile[i];
            if (repGap[i] < t.mGaps.size())
            {
                const GapEnclosure& ge = t.mGaps[repGap[i]];
                f->mSpace[i] = ge.mSpace;
                if (ge.mBucket == static_cast<U8>(EnclosureBucket::INDOORS))
                {
                    fl |= WindCell::INDOORS;
                }
            }
        }

        // Courtyards: open spaces whose edge at the slab is mostly solid.
        if (rooms)
        {
            SK_NAV_ZONE("nav.wind.field.courts");
            const size_t spaces = rooms->mSpaces.size();
            std::vector<U32> cells(spaces, 0), edge(spaces, 0), walled(spaces, 0);
            std::vector<F32> top(spaces, 0.f);      ///< summed over walled edge cells: the wall's top
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    const size_t i = x + static_cast<size_t>(y) * w;
                    const U32 sp = f->mSpace[i];
                    if (!sp || sp > spaces || (f->mFlags[i] & WindCell::BLOCKED))
                    {
                        continue;
                    }
                    ++cells[sp - 1];
                    bool is_edge = false, is_wall = false;
                    F32 wall_top = 0.f;
                    const S32 nb[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
                    for (const auto& q : nb)
                    {
                        const bool in = q[0] >= 0 && q[1] >= 0 && q[0] < w && q[1] < h;
                        const size_t j = in ? q[0] + static_cast<size_t>(q[1]) * w : 0;
                        if (!in || f->mSpace[j] != sp)
                        {
                            is_edge = true;
                            if (in && (f->mFlags[j] & WindCell::BLOCKED))
                            {
                                is_wall = true;
                                wall_top = std::max(wall_top, f->mZ[j] + f->mSolid[j]);
                            }
                        }
                    }
                    edge[sp - 1] += is_edge ? 1 : 0;
                    walled[sp - 1] += is_wall ? 1 : 0;
                    top[sp - 1] += is_wall ? wall_top : 0.f;
                }
            }
            std::vector<bool> court(spaces, false);
            for (size_t k = 0; k < spaces; ++k)
            {
                const Space& s = rooms->mSpaces[k];
                if (s.mBucket == EnclosureBucket::INDOORS || s.mSMean <= params.mCourtS || !walled[k] ||
                    walled[k] < params.mCourtL * edge[k] || cells[k] * cell * cell < params.mCourtAreaM2)
                {
                    continue;
                }
                // Narrow enough to lie in its walls' lee: a walled plaza wider than that is open ground.
                const F32 wall = top[k] / walled[k] - s.mFloorMin;
                const F32 lee = std::min(params.mLeePerHeight * std::max(wall, params.mLeeMinHeightM), params.mLeeCapM);
                court[k] = 2.f * std::min(s.mHalf.mV[0], s.mHalf.mV[1]) <= lee;
                if (court[k])
                {
                    f->mCourt.push_back(static_cast<U32>(k));
                }
            }
            for (size_t i = 0; i < n; ++i)
            {
                if (f->mSpace[i] && f->mSpace[i] <= spaces && court[f->mSpace[i] - 1] && !(f->mFlags[i] & WindCell::BLOCKED))
                {
                    f->mFlags[i] |= WindCell::COURTYARD;
                }
            }
        }
        reachField(*f);
        return f;
    }

    std::vector<F32> windWeightBlur(const WindField& f, const WindParams& params)
    {
        std::vector<F32> out;
        blur(windWeight(f), gaussian(params.mSmoothM / f.mCell), f.mWidth, f.mHeight, out);
        return out;
    }

    std::shared_ptr<WindSlab> computeWindSlab(const WindField& f, const RoomsProduct* rooms, F32 heading_deg, const WindParams& params,
                                              const std::vector<F32>* weight_blur)
    {
        SK_NAV_ZONE("nav.wind.slab");
        auto slab = std::make_shared<WindSlab>();
        slab->mOrigin = f.mOrigin;
        slab->mCell = f.mCell;
        slab->mWidth = f.mWidth;
        slab->mHeight = f.mHeight;
        const S32 w = f.mWidth, h = f.mHeight;
        const size_t n = static_cast<size_t>(w) * h;
        slab->mCells.resize(n);
        if (!n)
        {
            return slab;
        }
        const F32 cell = f.mCell;
        const F32 rad = heading_deg * DEG_TO_RAD;
        const LLVector2 wd(std::cos(rad), std::sin(rad)), pp(-wd.mV[VY], wd.mV[VX]);
        const F32 ox = f.mOrigin.mV[VX], oy = f.mOrigin.mV[VY];
        // Dividing by a power of two is multiplying by its reciprocal, exactly.
        S32 cell_exp = 0;
        const bool pow2 = std::frexp(cell, &cell_exp) == 0.5f;
        const F32 per_cell = 1.f / cell;
        auto index = [&](F32 x, F32 y) -> S32 {
            const F32 fx = pow2 ? (x - ox) * per_cell : (x - ox) / cell, fy = pow2 ? (y - oy) * per_cell : (y - oy) / cell;
            const S32 ix = static_cast<S32>(std::floor(fx)), iy = static_cast<S32>(std::floor(fy));
            return ix < 0 || iy < 0 || ix >= w || iy >= h ? -1 : ix + iy * w;
        };
        auto blocked = [&](S32 i) { return (f.mFlags[i] & WindCell::BLOCKED) != 0; };

        // Lee: the least of s / min(8 h, cap) over solids s upwind.
        SK_NAV_ZONE("nav.wind.slab.lee");
        std::vector<F32> lee(n, 1.f), width(n, 0.f);
        const F32 cap = params.mLeeCapM;
        // Per cell, what the marches read: the cells a march skips from an open one, nothing on a blocked one, and
        // a blocked one's lee length.
        std::vector<S32> hop(n);
        std::vector<F32> lee_len(n, 0.f);
        for (size_t j = 0; j < n; ++j)
        {
            if (f.mFlags[j] & WindCell::BLOCKED)
            {
                lee_len[j] = std::min(params.mLeePerHeight * std::max(f.mSolid[j], params.mLeeMinHeightM), cap);
            }
            else
            {
                hop[j] = skip(f.mReach[j]);
            }
        }
        for (S32 y = 0; y < h; ++y)
        {
            for (S32 x = 0; x < w; ++x)
            {
                const size_t i = x + static_cast<size_t>(y) * w;
                if (blocked(static_cast<S32>(i)))
                {
                    lee[i] = 0.f;
                    continue;
                }
                const F32 px = ox + (x + 0.5f) * cell, py = oy + (y + 0.5f) * cell;
                F32 best = 1.f;
                for (F32 s = cell; s < SWEEP_M && s / cap < best;)
                {
                    const S32 j = index(px - wd.mV[VX] * s, py - wd.mV[VY] * s);
                    if (j < 0)
                    {
                        break;      // the raster is convex: nothing further is on it
                    }
                    if (!hop[j])
                    {
                        best = std::min(best, s / lee_len[j]);
                        s += cell;
                    }
                    else
                    {
                        s += hop[j] * cell;
                    }
                }
                lee[i] = best;

                // Free width across the wind, both sides.
                F32 sum = 0.f;
                for (F32 sgn : { 1.f, -1.f })
                {
                    F32 span = params.mWidthReachM;
                    for (F32 s = cell; s <= params.mWidthReachM + 1e-3f;)
                    {
                        const S32 j = index(px + sgn * pp.mV[VX] * s, py + sgn * pp.mV[VY] * s);
                        if (j < 0)
                        {
                            break;
                        }
                        if (!hop[j])
                        {
                            span = s;
                            break;
                        }
                        s += hop[j] * cell;
                    }
                    sum += span;
                }
                width[i] = sum;
            }
        }
        // The weight is the same for every heading, so a pass may blur it once.
        smooth(width, windWeight(f), weight_blur ? *weight_blur : windWeightBlur(f, params), w, h, params.mSmoothM / cell);

        // Venturi, calm indoors, damped and swirling courtyards.
        SK_NAV_ZONE("nav.wind.slab.venturi");
        const F32 fu = 1.f / WIND_FACTOR_UNIT, du = 1.f / WIND_DEFLECT_UNIT;
        auto put = [&](WindCell& c, F32 speed, const LLVector2& deflect) {
            c.mFactor = static_cast<U8>(std::clamp<S32>(static_cast<S32>(std::lround(speed * fu)), 0, 255));
            c.mDeflect[0] = static_cast<S8>(std::clamp<S32>(static_cast<S32>(std::lround(deflect.mV[VX] * du)), -127, 127));
            c.mDeflect[1] = static_cast<S8>(std::clamp<S32>(static_cast<S32>(std::lround(deflect.mV[VY] * du)), -127, 127));
        };
        std::vector<F32> speed(n, 0.f);
        for (S32 y = 0; y < h; ++y)
        {
            for (S32 x = 0; x < w; ++x)
            {
                const size_t i = x + static_cast<size_t>(y) * w;
                WindCell& c = slab->mCells[i];
                c.mFlags = f.mFlags[i];
                if (blocked(static_cast<S32>(i)))
                {
                    continue;
                }
                const F32 px = ox + (x + 0.5f) * cell, py = oy + (y + 0.5f) * cell;
                F32 up = 0.f;
                for (F32 s : params.mUpwindM)
                {
                    const S32 j = index(px - wd.mV[VX] * s, py - wd.mV[VY] * s);
                    up += j < 0 ? params.mOpenWidthM : width[j];
                }
                up /= static_cast<F32>(std::size(params.mUpwindM));
                F32 v = lee[i] * std::clamp(up / std::max(width[i], 0.25f), params.mVenturiMin, params.mVenturiMax);
                LLVector2 deflect;
                if (c.mFlags & WindCell::INDOORS)
                {
                    v = 0.f;
                }
                else if ((c.mFlags & WindCell::COURTYARD) && rooms && f.mSpace[i] && f.mSpace[i] - 1u < rooms->mSpaces.size())
                {
                    v *= params.mCourtDamp;
                    const Space& sp = rooms->mSpaces[f.mSpace[i] - 1];
                    const LLVector2 r(px - sp.mCentre.mV[VX], py - sp.mCentre.mV[VY]);
                    const F32 len = r.length();
                    if (len > 1e-3f)
                    {
                        // A slow counter-clockwise turn about the courtyard's centre.
                        deflect = LLVector2(-r.mV[VY], r.mV[VX]) * (params.mSwirl * std::min(1.f, len / 2.f) / len);
                    }
                }
                speed[i] = v;
                put(c, v, deflect);
            }
        }

        // Draughts through portals whose sides differ in S, where the slab crosses them.
        if (rooms)
        {
            SK_NAV_ZONE("nav.wind.slab.draughts");
            for (const Portal& p : rooms->mPortals)
            {
                if (p.mSpaces[0] >= rooms->mSpaces.size() || p.mSpaces[1] >= rooms->mSpaces.size())
                {
                    continue;
                }
                const F32 sa = rooms->mSpaces[p.mSpaces[0]].mSMean, sb = rooms->mSpaces[p.mSpaces[1]].mSMean;
                const S32 ci = index(p.mCentre.mV[VX], p.mCentre.mV[VY]);
                if (std::fabs(sa - sb) < params.mDraughtS || ci < 0)
                {
                    continue;
                }
                const F32 top = p.lintel() && p.mHead > p.mSill ? p.mHead : p.mSill + 2.5f;
                if (f.mZ[ci] < p.mSill - 0.5f || f.mZ[ci] > top)
                {
                    continue;
                }
                const LLVector2 nrm(p.mNormal.mV[VX], p.mNormal.mV[VY]);
                const F32 d = wd * nrm;
                // With the wind along the portal, air still moves from the open side in.
                const LLVector2 dir = d > 0.2f ? nrm : d < -0.2f ? -nrm : (sa > sb ? nrm : -nrm);
                const F32 strength = params.mDraughtSpeed * (0.4f + 0.6f * std::fabs(d));
                const U32 inner = sa > sb ? p.mSpaces[1] : p.mSpaces[0];
                const F32 reach = p.mWidth * 0.5f + params.mDraughtReachM;
                const S32 r = static_cast<S32>(std::ceil(reach / cell));
                const S32 cx = ci % w, cy = ci / w;
                for (S32 y = std::max(0, cy - r); y <= std::min(h - 1, cy + r); ++y)
                {
                    for (S32 x = std::max(0, cx - r); x <= std::min(w - 1, cx + r); ++x)
                    {
                        const size_t i = x + static_cast<size_t>(y) * w;
                        const U16 sp = f.mSpace[i];
                        if (blocked(static_cast<S32>(i)) || (sp != p.mSpaces[0] + 1 && sp != p.mSpaces[1] + 1))
                        {
                            continue;
                        }
                        const F32 dist = LLVector2(ox + (x + 0.5f) * cell - p.mCentre.mV[VX], oy + (y + 0.5f) * cell - p.mCentre.mV[VY]).length();
                        const F32 v = strength * (1.f - dist / reach);
                        if (dist >= reach)
                        {
                            continue;
                        }
                        WindCell& c = slab->mCells[i];
                        c.mFlags |= WindCell::DRAUGHT;
                        // On the sheltered side the air runs through the portal, at least at the draught's speed.
                        if (sp == inner + 1 || v > speed[i])
                        {
                            speed[i] = std::max(speed[i], v);
                            put(c, speed[i], dir - wd);
                        }
                    }
                }
            }
        }
        return slab;
    }

    struct WindPass::State
    {
        std::shared_ptr<const WindInput> mInput;
        std::shared_ptr<WindProduct> mProduct;
        size_t mBand = 0;
        S32 mHeading = -1;          ///< -1 while the band's field is still to build
        WindFieldPtr mField;
        std::shared_ptr<const std::vector<F32>> mZ;
        std::vector<F32> mWeightBlur;
        F64 mMs = 0.0;
        bool mDone = false;
    };

    WindPass::WindPass(std::shared_ptr<const WindInput> input)
        : mState(std::make_unique<State>())
    {
        mState->mInput = std::move(input);
        mState->mProduct = std::make_shared<WindProduct>();
        mState->mProduct->mGeneration = mState->mInput->mGeneration;
        mState->mProduct->mBands = mState->mInput->mBands;
        mState->mDone = !mState->mInput->mEnclosure || mState->mInput->mBands.empty();
    }

    WindPass::~WindPass() = default;

    bool WindPass::step()
    {
        State& s = *mState;
        if (s.mDone)
        {
            return true;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const WindInput& in = *s.mInput;
        if (s.mHeading < 0)
        {
            s.mField = buildWindField(*in.mEnclosure, in.mRooms.get(), in.mBands[s.mBand], in.mParams);
            s.mZ = std::make_shared<const std::vector<F32>>(s.mField->mZ);
            s.mWeightBlur = windWeightBlur(*s.mField, in.mParams);
            s.mHeading = 0;
        }
        else
        {
            std::shared_ptr<WindSlab> slab = computeWindSlab(*s.mField, in.mRooms.get(), windHeadingDeg(s.mHeading), in.mParams, &s.mWeightBlur);
            slab->mHeading = s.mHeading;
            slab->mBand = static_cast<S32>(s.mBand);
            slab->mZ = s.mZ;
            s.mProduct->mSlabs.push_back(slab);
            if (++s.mHeading == WIND_HEADINGS)
            {
                s.mHeading = -1;
                s.mField.reset();
                s.mWeightBlur.clear();
                s.mDone = ++s.mBand == in.mBands.size();
            }
        }
        s.mMs += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t0).count();
        s.mProduct->mMillis = static_cast<F32>(s.mMs);
        return s.mDone;
    }

    bool WindPass::done() const
    {
        return mState->mDone;
    }

    WindProductPtr WindPass::product() const
    {
        return mState->mProduct;
    }

    std::vector<WindLevel> defaultWindBands()
    {
        WindLevel low, high;
        low.mOffsetM = 1.5f;
        high.mOffsetM = 10.f;
        return { low, high };
    }

    WindProductPtr computeWind(std::shared_ptr<const WindInput> input)
    {
        WindPass pass(std::move(input));
        while (!pass.step())
        {
        }
        return pass.product();
    }
}
