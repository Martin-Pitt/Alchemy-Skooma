/**
 * @file skroadsigns.h
 * @brief Spike: road signs known by their texture, from the sign library the survey's textures were read into.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#pragma once

#include "lluuid.h"

#include <string>

namespace skroad
{
    // What a sign texture says (V:\Scratch\worldmap\signs\sign-library.json's "class").
    enum class SignClass : U8
    {
        None,
        RoadClosed,
        RoadEnds,
        DeadEnd,
        NoThroughRoad,
        Stop,
        GiveWay,
        SpeedLimit,
        JunctionWarning,
        CurveWarning,
        Danger,
        OneWay,
        RezZone,
        Other,
    };

    struct Sign
    {
        SignClass what = SignClass::None;
        F32 value = 0.f;            // a speed limit's number as posted (its unit is the sign's)
        std::string text;
    };

    // Loads the library at path (empty: none), keyed by texture id; kept until the path changes. False where it
    // could not be read.
    bool loadSignLibrary(const std::string& path);
    const Sign* signFor(const LLUUID& texture);
    SignClass signClassNamed(const std::string& name);
}
