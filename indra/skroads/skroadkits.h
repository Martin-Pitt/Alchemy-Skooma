/**
 * @file skroadkits.h
 * @brief Spike: road kits by their geometry signature, and the measured kit library (drivable deck, kerbs, lanes in
 * each kit's own frame) that places a known kit's edges and lanes in place of the generic lane rule.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#pragma once

#include "llmath.h"
#include "llquaternion.h"
#include "v3math.h"

#include <string>
#include <vector>

namespace skroad
{
    // One prim of a linkset as the signature reads it: its shape, scale, and pose in the root's frame.
    struct KitPrim
    {
        std::string shape;          // kitVolumeShape or kitSculptShape
        LLVector3 scale;
        LLVector3 pos;              // in the root's frame (zero for the root)
        LLQuaternion rot;           // likewise (identity for the root)
        bool root = false;
    };
    // vp: profile curve, begin, end, hollow; path curve, begin, end, scale x, y, shear x, y, twist begin, twist.
    std::string kitVolumeShape(const F64 vp[13]);
    std::string kitSculptShape(const std::string& sculpt_id, S32 type);
    // The linkset's geometry signature: skroadkitsnaps.cpp's signature without the root name ("#<children>" and the
    // prims' entries sorted, one a line), and its 64-bit FNV-1a hash, which keys the library.
    std::string kitGeoSignature(const std::vector<KitPrim>& prims);
    U64 kitHash(const std::string& s);

    // A kit as measured in its root's frame (V:\Scratch\worldmap\kits\lane-library.json): the deck faces up along
    // `up` (axis 0..2, sign), runs along `along`; distances are along `across` from the root's origin.
    struct Kit
    {
        std::string name, confidence;
        S32 up = 2, up_sign = 1, across = 0, along = 1;
        F64 deck_min = 0, deck_max = 0;     // the drivable deck
        F64 kerb_min = 0, kerb_max = 0;     // with its kerbs and sides
        std::vector<F64> lanes;             // lane centres
    };
    // Loads the library at path (empty: none); kept until the path changes. False where it could not be read.
    bool loadKitLibrary(const std::string& path);
    bool kitLibraryOn();
    const Kit* kitFor(U64 hash);
}
