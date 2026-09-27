/**
 * @file sknavwind.h
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

#ifndef SK_NAVWIND_H
#define SK_NAVWIND_H

#include "sknavenclosure.h"
#include "sknavspaces.h"

#include "v2math.h"

#include <memory>
#include <vector>

namespace sknav
{
    constexpr S32 WIND_HEADINGS = 16;       ///< heading buckets, 22.5 degrees apart

    /// A heading is where the wind blows toward, in degrees counter-clockwise from +x (east).
    inline F32 windHeadingDeg(S32 bucket) { return bucket * (360.f / WIND_HEADINGS); }

    struct WindParams
    {
        F32 mCellM = 0.5f;          ///< raster cell
        F32 mLeePerHeight = 8.f;    ///< speed recovers over this many heights of the solid upwind
        F32 mLeeCapM = 60.f;        ///< and never over more than this
        F32 mLeeMinHeightM = 0.5f;  ///< a solid counts as at least this tall
        F32 mWidthReachM = 16.f;    ///< free width is looked for this far to each side
        F32 mSmoothM = 2.f;         ///< Gaussian sigma on the width before the venturi ratio
        F32 mUpwindM[3] = { 6.f, 8.f, 10.f };   ///< the upwind width is the mean of these
        F32 mOpenWidthM = 32.f;     ///< the width read upwind of the raster
        F32 mVenturiMin = 0.5f;
        F32 mVenturiMax = 2.f;
        F32 mBlockedShare = 0.25f;  ///< a cell is solid when this much of it is
        F32 mDraughtS = 0.3f;       ///< a portal draughts when its sides' mean S differ this much
        F32 mDraughtReachM = 2.f;   ///< past its half width
        F32 mDraughtSpeed = 0.5f;   ///< at its centre, wind square on
        F32 mCourtS = 0.5f;         ///< an outdoor space this open to the sky
        F32 mCourtL = 0.6f;         ///< and this much of its edge at the slab solid is a courtyard
        F32 mCourtAreaM2 = 16.f;
        F32 mCourtDamp = 0.5f;
        F32 mSwirl = 0.35f;         ///< the courtyard's turn, in free-stream units
    };

    /// Where a slab lies: offset metres above the ground, or at a fixed
    /// mesh-local height when absolute.
    struct WindLevel
    {
        F32 mOffsetM = 1.5f;
        bool mAbsolute = false;
        F32 mZ = 0.f;
    };

    /// One cell of a slab. The local wind is mFactor times the free stream's
    /// speed, blowing along the heading plus the deflection, normalised.
    struct WindCell
    {
        enum : U8
        {
            BLOCKED = 1 << 0,       ///< solid at the slab
            UNSEEN = 1 << 1,       ///< no detailed air here
            INDOORS = 1 << 2,
            COURTYARD = 1 << 3,
            DRAUGHT = 1 << 4,       ///< near a portal between sides that differ in S
        };
        U8 mFactor = 0;             ///< speed in WIND_FACTOR_UNIT
        S8 mDeflect[2] = { 0, 0 };  ///< in WIND_DEFLECT_UNIT, x and y
        U8 mFlags = 0;

        F32 factor() const;
        LLVector2 deflection() const;
    };

    constexpr F32 WIND_FACTOR_UNIT = 1.f / 120.f;
    constexpr F32 WIND_DEFLECT_UNIT = 1.f / 60.f;

    /// What every heading of one band shares: the obstacles and the air at the slab.
    struct WindField
    {
        LLVector2 mOrigin;          ///< mesh-local corner
        F32 mCell = 0.5f;
        S32 mWidth = 0;
        S32 mHeight = 0;
        WindLevel mLevel;
        std::vector<F32> mZ;        ///< the slab's mesh-local height per cell
        std::vector<U8> mFlags;     ///< WindCell BLOCKED, UNSEEN, INDOORS, COURTYARD
        std::vector<F32> mSolid;    ///< metres of solid above the slab, on blocked cells
        std::vector<F32> mReach;    ///< a lower bound, in cells, on the distance to a blocked cell
        std::vector<U16> mSpace;    ///< the slab's gap's space + 1
        std::vector<U32> mCourt;    ///< courtyard spaces, sorted, indices into the rooms product
    };
    typedef std::shared_ptr<const WindField> WindFieldPtr;

    /// One heading of one band. Positions are mesh-local.
    struct WindSlab
    {
        S32 mHeading = 0;           ///< bucket
        S32 mBand = 0;
        LLVector2 mOrigin;
        F32 mCell = 0.5f;
        S32 mWidth = 0;
        S32 mHeight = 0;
        std::shared_ptr<const std::vector<F32>> mZ;     ///< the slab's height per cell, shared by the band
        std::vector<WindCell> mCells;                   ///< row-major along y

        const WindCell* at(F32 x, F32 y) const;
        size_t bytes() const;
    };
    typedef std::shared_ptr<const WindSlab> WindSlabPtr;

    /// Every slab of a mesh: WIND_HEADINGS per band, heading fastest.
    struct WindProduct
    {
        U64 mGeneration = 0;
        std::vector<WindLevel> mBands;
        std::vector<WindSlabPtr> mSlabs;
        F32 mMillis = 0.f;

        WindSlabPtr slab(S32 heading, S32 band) const;
        size_t bytes() const;
    };
    typedef std::shared_ptr<const WindProduct> WindProductPtr;

    /// The obstacles and air of a band over the enclosure product's detailed
    /// tiles. The ground under a slab that follows it is the lowest terrain
    /// floor, carried under objects from the nearest terrain, or the mesh's
    /// lowest floor when it has no terrain.
    WindFieldPtr buildWindField(const EnclosureProduct& enclosure, const RoomsProduct* rooms, const WindLevel& level, const WindParams& params);

    /// One heading over a field: lee behind solids, venturi from the smoothed
    /// free width, calm indoors, damped and swirling courtyards, and draughts
    /// at portals whose sides differ in S.
    std::shared_ptr<WindSlab> computeWindSlab(const WindField& field, const RoomsProduct* rooms, F32 heading_deg, const WindParams& params);

    struct WindInput
    {
        U64 mGeneration = 0;
        EnclosureProductPtr mEnclosure;
        RoomsProductPtr mRooms;
        std::vector<WindLevel> mBands;
        WindParams mParams;
    };

    /// Every slab of a mesh, one field or slab per step.
    class WindPass
    {
    public:
        explicit WindPass(std::shared_ptr<const WindInput> input);
        ~WindPass();

        bool step();
        bool done() const;
        WindProductPtr product() const;

    private:
        struct State;
        std::unique_ptr<State> mState;
    };

    /// Everything at once, for tests and tools.
    WindProductPtr computeWind(std::shared_ptr<const WindInput> input);
}

#endif // SK_NAVWIND_H
