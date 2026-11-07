/**
 * @file skroadkits.cpp
 * @brief Spike: road kits by their geometry signature, and the measured kit library.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "skroadkits.h"

#include "llerror.h"
#include "llstring.h"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace skroad
{
namespace
{
    std::string sPath;
    std::unordered_map<U64, Kit> sKits;

    // As skroadkitsnaps.cpp's num(): rounded to the step, printed %g, never -0.
    std::string num(F64 v, F64 step)
    {
        const F64 r = std::round(v / step) * step;
        return llformat("%g", std::abs(r) < step * 0.5 ? 0.0 : r);
    }

    F64 real(simdjson::dom::element e, F64 fallback = 0)
    {
        double d;
        if (!e.get_double().get(d))
        {
            return d;
        }
        int64_t i;
        return e.get_int64().get(i) ? fallback : F64(i);
    }
}

std::string kitVolumeShape(const F64 vp[13])
{
    return llformat("P%d,%s,%s,%s;%d,%s,%s,%s,%s,%s,%s,%s,%s", (S32)vp[0], num(vp[1], 0.001).c_str(), num(vp[2], 0.001).c_str(),
                    num(vp[3], 0.001).c_str(), (S32)vp[4], num(vp[5], 0.001).c_str(), num(vp[6], 0.001).c_str(),
                    num(vp[7], 0.001).c_str(), num(vp[8], 0.001).c_str(), num(vp[9], 0.001).c_str(), num(vp[10], 0.001).c_str(),
                    num(vp[11], 0.001).c_str(), num(vp[12], 0.001).c_str());
}

std::string kitSculptShape(const std::string& sculpt_id, S32 type)
{
    return llformat("S%s/%d", sculpt_id.substr(0, 8).c_str(), type);
}

std::string kitGeoSignature(const std::vector<KitPrim>& prims)
{
    std::vector<std::string> entries;
    entries.reserve(prims.size());
    for (const KitPrim& p : prims)
    {
        const LLVector3 pos = p.root ? LLVector3::zero : p.pos;
        LLQuaternion q = p.root ? LLQuaternion::DEFAULT : p.rot;
        if (q.mQ[VW] < 0.f)
        {
            q = LLQuaternion(-q.mQ[VX], -q.mQ[VY], -q.mQ[VZ], -q.mQ[VW]);
        }
        entries.push_back(p.shape + "|" + num(p.scale.mV[VX], 0.01) + "," + num(p.scale.mV[VY], 0.01) + "," + num(p.scale.mV[VZ], 0.01)
                          + "|" + num(pos.mV[VX], 0.1) + "," + num(pos.mV[VY], 0.1) + "," + num(pos.mV[VZ], 0.1) + "|"
                          + num(q.mQ[VX], 0.01) + "," + num(q.mQ[VY], 0.01) + "," + num(q.mQ[VZ], 0.01) + "," + num(q.mQ[VW], 0.01));
    }
    std::sort(entries.begin(), entries.end());
    std::string sig = "#" + std::to_string(S32(prims.size()) - 1);
    for (const std::string& e : entries)
    {
        sig += "\n" + e;
    }
    return sig;
}

U64 kitHash(const std::string& s)
{
    U64 h = 14695981039346656037ull;
    for (unsigned char c : s)
    {
        h = (h ^ c) * 1099511628211ull;
    }
    return h;
}

bool loadKitLibrary(const std::string& path)
{
    if (path == sPath)
    {
        return !sKits.empty() || path.empty();
    }
    sPath = path;
    sKits.clear();
    if (path.empty())
    {
        return true;
    }
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    simdjson::dom::object kits;
    if (parser.load(path).get(doc) || doc["kits"].get_object().get(kits))
    {
        LL_WARNS("SKRoadKits") << "no kit library at " << path << LL_ENDL;
        return false;
    }
    for (auto [key, v] : kits)
    {
        Kit k;
        std::string_view s;
        if (!v["name"].get_string().get(s)) k.name = std::string(s);
        if (!v["confidence"].get_string().get(s)) k.confidence = std::string(s);
        k.up = S32(real(v["up"]["axis"], 2));
        k.up_sign = real(v["up"]["sign"], 1) < 0 ? -1 : 1;
        k.across = S32(real(v["across_axis"], 0));
        k.along = S32(real(v["along_axis"], 1));
        k.deck_min = real(v["deck"].at(0));
        k.deck_max = real(v["deck"].at(1));
        k.kerb_min = real(v["kerb"].at(0), k.deck_min);
        k.kerb_max = real(v["kerb"].at(1), k.deck_max);
        simdjson::dom::array lanes;
        if (!v["lanes"].get_array().get(lanes))
        {
            for (simdjson::dom::element c : lanes)
            {
                k.lanes.push_back(real(c));
            }
        }
        if (k.deck_max - k.deck_min < 2.0 || k.across < 0 || k.across > 2)
        {
            continue;
        }
        sKits[std::strtoull(std::string(key).c_str(), nullptr, 16)] = std::move(k);
    }
    LL_INFOS("SKRoadKits") << sKits.size() << " kits from " << path << LL_ENDL;
    return !sKits.empty();
}

bool kitLibraryOn()
{
    return !sKits.empty();
}

const Kit* kitFor(U64 hash)
{
    auto it = sKits.find(hash);
    return it == sKits.end() ? nullptr : &it->second;
}
}
