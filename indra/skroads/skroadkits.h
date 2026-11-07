/**
 * @file skroadkits.h
 * @brief Spike: road kits by their geometry signature, measured from their own geometry (drivable deck, kerbs, lanes
 * in each kit's own frame), which places a known kit's edges and lanes in place of the generic lane rule.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#pragma once

#include "llmath.h"
#include "llquaternion.h"
#include "llvolume.h"
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
    // prims' entries sorted, one a line), and its 64-bit FNV-1a hash.
    std::string kitGeoSignature(const std::vector<KitPrim>& prims);
    U64 kitHash(const std::string& s);

    // A kit as measured in its root's frame: the deck faces up along `up` (axis 0..2, sign), runs along `along`;
    // distances are along `across` from the root's origin.
    struct Kit
    {
        std::string confidence;             // "high": one width along the piece and a clear along axis
        S32 up = 2, up_sign = 1, across = 0, along = 1;
        F64 deck_min = 0, deck_max = 0;     // the drivable deck
        F64 kerb_min = 0, kerb_max = 0;     // with its kerbs and sides
        F64 crown = 0;                      // the deck's height range across it
        std::vector<F64> lanes;             // lane centres, from the deck's edges
    };

    // A linkset's geometry for the measurement, in its root's frame.
    struct KitShapePrim
    {
        LLVolumeParams params;              // a prim's, built here at the top detail
        std::vector<LLVector3> tris;        // or its triangles placed in the root's frame (mesh, sculpt), three a triangle
        LLVector3 scale, pos;               // the prim's, in the root's frame (unused with tris)
        LLQuaternion rot;
    };
    struct KitShape
    {
        std::vector<KitShapePrim> prims;
        LLQuaternion root_rot;              // the root's rotation in the region: which of its axes is up
    };

    // Measures a linkset as a road kit (ownframe.py's deck walk on three slices across it). Pure: any thread. False,
    // with why, where it is not one (no deck, too narrow, a deck that changes width along the piece).
    bool measureKit(const KitShape& shape, Kit& out, std::string* why = nullptr);

    // The measured kits, keyed by kitKey: one copy's measurement serves every copy of a kit lying the same way up.
    U64 kitKey(U64 hash, const LLQuaternion& root_rot);
    const Kit* kitFor(U64 key);             // null until measured, and where it is not a kit
    bool kitAsked(U64 key);                 // a measurement was asked for (pending or done)
    bool kitDone(U64 key);                  // measured, a kit or not
    void kitAsk(U64 key);
    void kitMeasured(U64 key, bool ok, const Kit& kit);
    void forgetKits();
}
