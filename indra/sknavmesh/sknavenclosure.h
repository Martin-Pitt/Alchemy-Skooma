/**
 * @file sknavenclosure.h
 * @brief How enclosed each air gap of a mesh is: sky openness, depth from outside and indoorness.
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

#ifndef SK_NAVENCLOSURE_H
#define SK_NAVENCLOSURE_H

#include "sknavairdistil.h"
#include "sknavskymap.h"
#include "sknavtypes.h"

#include <functional>
#include <memory>
#include <vector>

namespace sknav
{
    /// OUTDOORS < 0.25 <= SHELTERED <= 0.75 < INDOORS, by e. UNKNOWN is no
    /// answer: nothing reached the gap, and it is never to be read as outdoors.
    enum class EnclosureBucket : U8 { OUTDOORS = 0, SHELTERED = 1, INDOORS = 2, UNKNOWN = 3 };

    struct EnclosureParams
    {
        F32 mSkyDecayM = 2.f;       ///< S = exp(-d / this), d the lateral distance through air to open sky
        F32 mDepthFullM = 4.f;      ///< walkable depth at which e reaches 1 - S
        F32 mClimbM = 0.75f;        ///< floors of linked gaps within this
        F32 mWalkHeadM = 1.f;       ///< a walkable gap at least this tall carries feet
        F32 mWalkOverlapM = 1.f;    ///< and links to one sharing this much air
        F32 mCarryOverlapM = 0.5f;  ///< depth carries into unwalkable air sharing this much, at any step
        F32 mCarryRiseM = 2.f;      ///< and whose floor is within this of the walkable floor it came from
        F32 mSkyReachM = 50.f;      ///< solids overhead out to this far sideways hide a gap's sky
        F32 mHysteresis = 0.05f;    ///< a bucket holds until e is this far past its edge
        /// A gap roofed over at least this share and open at the sides on at
        /// least mShelterOpen of its spokes is SHELTERED however deep: a canopy, not a hall.
        F32 mShelterCover = 0.3f;
        F32 mShelterOpen = 0.55f;
        /// Walkable air no walkable path reaches from outside reads INDOORS,
        /// flagged sealed with D at its cap, rather than UNKNOWN.
        bool mSealedIndoors = true;
    };

    constexpr U8 ENCLOSURE_D_UNKNOWN = 0xff;
    constexpr F32 ENCLOSURE_D_UNIT_M = 0.25f;
    constexpr U8 ENCLOSURE_D_DEEP = ENCLOSURE_D_UNKNOWN - 1;   ///< the cap, what a sealed gap reads
    constexpr U8 ENCLOSURE_SEALED = 1 << 0;                     ///< reached from a sealed floor, not from outside
    constexpr U8 ENCLOSURE_CANOPY = 1 << 1;                     ///< roofed but open at the sides: capped at SHELTERED

    /// One gap's answer, parallel to its TileAir's gaps.
    struct GapEnclosure
    {
        U16 mSpace = 0;             ///< the product's space + 1, 0 for none (sknavspaces.h)
        U8 mS = 0;                  ///< S in 1/255
        U8 mE = 0;                  ///< e in 1/255
        U8 mD = ENCLOSURE_D_UNKNOWN; ///< depth in ENCLOSURE_D_UNIT_M
        U8 mBucket = static_cast<U8>(EnclosureBucket::UNKNOWN);
        U8 mFlags = 0;              ///< ENCLOSURE_SEALED
        U8 mCover = 0;              ///< share of the sky roofed over, walls aside, in 1/255
        U8 mOpen = 0;               ///< O, the share of spokes no wall crosses at eye height on their way under open sky, in 1/255; 0 unread
    };

    /// A 1 m cell's gaps as a far reader needs them, kept when detail is not.
    /// Heights are in the tile's units, like its air.
    struct CoarseGap
    {
        U16 mFloor = 0;
        U16 mCeiling = 0;           ///< AIR_CEILING_OPEN when nothing is above
        U16 mSpace = 0;             ///< the space most of its floor is in, + 1
        U8 mS = 0;
        U8 mE = 0;
        U8 mD = ENCLOSURE_D_UNKNOWN;
        U8 mFlags = 0;              ///< ENCLOSURE_SEALED, ENCLOSURE_CANOPY when most of its known air is
        U8 mOpen = 0;
        U8 mBucket = static_cast<U8>(EnclosureBucket::UNKNOWN);   ///< of its mean e, with the same hysteresis
    };

    struct CoarseAir
    {
        S32 mCells = 1;             ///< fine cells per coarse cell a side
        S32 mWidth = 0;
        S32 mHeight = 0;
        std::vector<U32> mStart;    ///< mWidth * mHeight + 1, row-major along y
        std::vector<CoarseGap> mGaps;
    };

    /// A read at a point: all zero and UNKNOWN when there is no answer.
    struct Enclosure
    {
        F32 mS = 0.f;
        F32 mD = 0.f;
        F32 mE = 0.f;
        F32 mO = 0.f;               ///< side openness
        EnclosureBucket mBucket = EnclosureBucket::UNKNOWN;
        U16 mSpace = 0;             ///< the product's space + 1, 0 for none
        U32 mSpaceId = 0;           ///< that space's id, stable within its mesh
        bool mSealed = false;       ///< INDOORS because no way in was found, D at its cap
        bool mCanopy = false;       ///< SHELTERED rather than INDOORS because roofed but open at the sides
        bool mFine = false;         ///< answered from detail air finer than the coarse cell
        bool mAvatarFloor = false;  ///< AVATAR walks the solid under it; fine air only
        F32 mFloor = 0.f;           ///< the gap's, in the frame the read was asked in
        F32 mCeiling = 0.f;         ///< a very large number when open
    };

    struct TileEnclosure
    {
        TileKey mKey;
        LLVector3 mOrigin;          ///< mesh-local, the air's
        F32 mCell = 0.f;
        F32 mUnit = 0.f;
        S32 mWidth = 0;
        S32 mHeight = 0;
        TileAirPtr mAir;            ///< the detail answered from; null once evicted
        std::vector<GapEnclosure> mGaps;
        CoarseAir mCoarse;

        size_t bytes() const;
    };

    /// One mesh's answers. Positions are mesh-local.
    struct SkyMemo;
    struct EnclosureMemo;

    struct EnclosureProduct
    {
        U64 mGeneration = 0;
        U64 mSerial = 0;                       ///< unique to this product and never reused, unlike its address
        std::vector<TileEnclosure> mTiles;     ///< sorted by key
        U32 mGapCount = 0;
        U32 mUnknownGaps = 0;
        F64 mArea = 0.0;                       ///< m² of gap floor, summed over gaps
        F64 mUnknownArea = 0.0;
        U32 mSealedGaps = 0;
        F32 mMillis = 0.f;                     ///< worker time over all chunks
        U64 mSummary = 0;                      ///< hash of the coarse answers, what neighbours read
        std::vector<U32> mSpaceIds;            ///< per space the spaces pass cut, its id
        /// Each tile's sky reads, kept while its air and the sky map blocks they read stand.
        std::shared_ptr<const SkyMemo> mSkyMemo;
        /// Each tile's graph and raw distances, what the next pass repairs rather than redoes.
        std::shared_ptr<const EnclosureMemo> mMemo;

        const TileEnclosure* tile(const TileKey& key) const;
        /// The gap holding pos: detail where the tile has it, else coarse.
        bool enclosureAt(const LLVector3& pos, Enclosure& out) const;
        /// Every gap over (x, y), lowest first, with its answer.
        void gapsAt(F32 x, F32 y, const std::function<void(const Enclosure&)>& fn) const;
        size_t bytes() const;
    };
    typedef std::shared_ptr<const EnclosureProduct> EnclosureProductPtr;

    struct EnclosureInput
    {
        struct Tile
        {
            TileKey mKey;
            TileAirPtr mAir;
            U64 mSkyContributor = 0;    ///< what the tile gave the sky map
        };
        /// A neighbouring mesh's last answers, read one generation late.
        struct Neighbour
        {
            LLVector3d mMeshOrigin;
            EnclosureProductPtr mProduct;
        };

        U64 mGeneration = 0;
        LLVector3d mMeshOrigin;
        std::vector<Tile> mTiles;
        std::shared_ptr<const SkyMap> mSky;
        EnclosureProductPtr mPrevious;
        std::vector<Neighbour> mNeighbours;
        /// Tiles whose detail was evicted: their last coarse answers stand
        /// as they were, without the detail, until the detail comes back.
        std::vector<TileEnclosure> mKept;
        EnclosureParams mParams;
        bool mTimeSeams = false;    ///< time the seam stitching on its own, for seamMillis
    };

    /// The per-mesh pass, in steps so a worker can yield between them: the
    /// graph of leaf gaps with seams stitched, the sky flood for S, the
    /// walkable flood for D with its carry into unwalkable air, then e,
    /// buckets and the coarse product.
    class EnclosurePass
    {
    public:
        explicit EnclosurePass(std::shared_ptr<const EnclosureInput> input);
        ~EnclosurePass();

        /// Do about budget units of work; true once the product is ready.
        bool step(U32 budget);
        bool done() const;
        /// Worker milliseconds by stage, for the stats.
        F32 stageMillis(S32 stage) const;
        /// Of the graph stage, the seams' share; zero unless the input asked for it.
        F32 seamMillis() const;
        static const char* stageName(S32 stage);
        static constexpr S32 NUM_STAGES = 6;
        EnclosureProductPtr product() const;
        /// The product before anyone shares it, for the spaces pass to write
        /// each gap's space into.
        std::shared_ptr<EnclosureProduct> takeProduct();
        /// Whether it repaired the previous answers rather than starting over.
        bool repaired() const;
        /// Tiles whose answers it worked out again, rather than kept.
        U32 tilesAnswered() const;
        /// Whether any tile's air, or any answer the spaces pass reads, moved
        /// from the previous answers: false means the previous spaces still stand.
        bool layoutMoved() const;

    private:
        struct State;
        std::unique_ptr<State> mState;
    };

    /// A serial no other enclosure product has had.
    U64 nextEnclosureSerial();

    /// Copy the previous answers' spaces into a product over the same air, for
    /// when the spaces pass is skipped.
    void carrySpaces(EnclosureProduct& product, const EnclosureProduct& previous);

    /// Everything at once, for tests and tools.
    EnclosureProductPtr computeEnclosure(std::shared_ptr<const EnclosureInput> input);

    F32 enclosureE(F32 s, F32 d, F32 d_full);
    /// The bucket for e, holding the previous one until e is hysteresis past its edge.
    EnclosureBucket enclosureBucket(F32 e, EnclosureBucket previous, F32 hysteresis);
    /// The leaf covering a fine cell, or -1.
    S32 findAirLeaf(const TileAir& air, S32 ix, S32 iy);
}

#endif // SK_NAVENCLOSURE_H
