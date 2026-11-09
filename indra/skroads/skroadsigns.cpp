/**
 * @file skroadsigns.cpp
 * @brief Spike: road signs known by their texture.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "skroadsigns.h"

#include "llerror.h"

#include <simdjson.h>

#include <unordered_map>

namespace skroad
{
namespace
{
    std::string sPath;
    std::unordered_map<LLUUID, Sign> sSigns;
}

SignClass signClassNamed(const std::string& name)
{
    static const std::pair<const char*, SignClass> NAMES[] = {
        { "road_closed", SignClass::RoadClosed },         { "road_ends", SignClass::RoadEnds },
        { "dead_end", SignClass::DeadEnd },               { "no_through_road", SignClass::NoThroughRoad },
        { "stop", SignClass::Stop },                      { "give_way", SignClass::GiveWay },
        { "speed_limit", SignClass::SpeedLimit },         { "junction_warning", SignClass::JunctionWarning },
        { "curve_warning", SignClass::CurveWarning },     { "danger", SignClass::Danger },
        { "one_way", SignClass::OneWay },                 { "rez_zone", SignClass::RezZone },
        { "other", SignClass::Other },
    };
    for (const auto& [n, c] : NAMES)
    {
        if (name == n)
        {
            return c;
        }
    }
    return SignClass::None;
}

bool loadSignLibrary(const std::string& path)
{
    if (path == sPath)
    {
        return !sSigns.empty() || path.empty();
    }
    sPath = path;
    sSigns.clear();
    if (path.empty())
    {
        return true;
    }
    simdjson::dom::parser parser;
    simdjson::dom::object signs;
    if (parser.load(path).get_object().get(signs))
    {
        LL_WARNS("SKRoadSigns") << "no sign library at " << path << LL_ENDL;
        return false;
    }
    for (auto [key, v] : signs)
    {
        std::string_view s;
        Sign sign;
        if (!v["class"].get_string().get(s))
        {
            sign.what = signClassNamed(std::string(s));
        }
        if (!v["text"].get_string().get(s))
        {
            sign.text = std::string(s);
        }
        double value = 0;
        if (!v["value"].get_double().get(value))
        {
            sign.value = F32(value);
        }
        else if (int64_t iv = 0; !v["value"].get_int64().get(iv))
        {
            sign.value = F32(iv);
        }
        // a low-confidence reading is not acted on
        if (!v["confidence"].get_string().get(s) && s == "low")
        {
            continue;
        }
        LLUUID id;
        if (sign.what != SignClass::None && sign.what != SignClass::Other && id.set(std::string(key), false))
        {
            sSigns[id] = std::move(sign);
        }
    }
    LL_INFOS("SKRoadSigns") << sSigns.size() << " road sign textures from " << path << LL_ENDL;
    return true;
}

const Sign* signFor(const LLUUID& texture)
{
    auto it = sSigns.find(texture);
    return it == sSigns.end() ? nullptr : &it->second;
}
}
