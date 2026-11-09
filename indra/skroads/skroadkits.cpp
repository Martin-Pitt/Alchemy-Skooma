/**
 * @file skroadkits.cpp
 * @brief Spike: road kits by their geometry signature, measured from their own geometry.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "skroadkits.h"

#include "llerror.h"
#include "llpointer.h"
#include "llstring.h"
#include "llvolumemgr.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace skroad
{
namespace
{
    // As skroadkitsnaps.cpp's num(): rounded to the step, printed %g, never -0.
    std::string num(F64 v, F64 step)
    {
        const F64 r = std::round(v / step) * step;
        return llformat("%g", std::abs(r) < step * 0.5 ? 0.0 : r);
    }

    // ---------------------------------------------------------------- the measurement (ownframe.py)

    constexpr F32 NONE = -std::numeric_limits<F32>::infinity();    // no prim over a cell or sample
    constexpr F64 GRID = 0.1;               // the top envelope's cell, which finds the along axis
    constexpr F64 DU = 0.05;                // a slice's sample across
    constexpr F64 STEP = 0.08;              // the deck walk ends at a step up or down of more than this between samples,
    constexpr F64 SLOPE = 0.35;             // a slope of more than this over 0.25 m,
    constexpr F64 MAXDZ = 0.6;              // more than this from where it started,
    constexpr S32 GAP = 4;                  // or more than this many samples with no prim (terrain, not deck),
    constexpr F64 KERB = 0.05;              // or a rise of more than this within KERB_RUN samples: a kerb's foot
    constexpr S32 KERB_RUN = 10;
    constexpr F64 MIN_DECK = 2.0;           // a narrower deck is a strip, not a road
    constexpr F64 VARIES = 1.0;             // a deck whose width changes more along the piece is a transition
    constexpr F64 SAME_WIDTH = 0.3;         // ... and one changing less is one width
    constexpr F64 ONE_LANE = 5.5;           // a narrower deck has one lane in its middle
    constexpr F64 LANE_W = 3.7;             // a lane at least this wide each way
    constexpr S64 MAX_CELLS = 4 << 20;      // a linkset too big to be a road piece

    bool has(F32 h)
    {
        return h != NONE;
    }

    F64 median(std::vector<F64> v)
    {
        std::sort(v.begin(), v.end());
        const size_t n = v.size();
        return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    }

    // Raises the samples of H under the segment (u, z) a to b: H[i] holds u0 + i * DU.
    void sampleSegment(std::vector<F32>& H, F64 u0, F64 ua, F64 za, F64 ub, F64 zb)
    {
        const S32 n = S32(H.size());
        auto raise = [&](S32 i, F64 z)
        {
            if (i >= 0 && i < n && z > H[i])
            {
                H[i] = F32(z);
            }
        };
        raise(S32(std::lround((ua - u0) / DU)), za);
        raise(S32(std::lround((ub - u0) / DU)), zb);
        if (ua > ub)
        {
            std::swap(ua, ub);
            std::swap(za, zb);
        }
        const F64 du = ub - ua;
        for (S32 i = std::max<S32>(0, S32(std::ceil((ua - u0) / DU))), last = std::min<S32>(n - 1, S32(std::floor((ub - u0) / DU)));
             i <= last; ++i)
        {
            const F64 u = u0 + i * DU;
            raise(i, du < 1e-6 ? std::max(za, zb) : za + (zb - za) * (u - ua) / du);
        }
    }

    // Walks the deck out from c each way to where it ends; the last deck sample each way.
    void walkDeck(const std::vector<F32>& H, S32 c, S32& left, S32& right)
    {
        const S32 n = S32(H.size());
        const F64 h0 = H[c];
        for (S32 d : { -1, 1 })
        {
            S32 i = c, prev = c, gap = 0;
            while (i + d >= 0 && i + d < n)
            {
                i += d;
                if (!has(H[i]))
                {
                    if (++gap > GAP)
                    {
                        break;
                    }
                    continue;
                }
                gap = 0;
                const F64 dz = H[i] - H[prev];
                if (dz > STEP || dz < -STEP)
                {
                    break;
                }
                const S32 j = i - 5 * d;
                if (j >= 0 && j < n && has(H[j]) && std::fabs(H[i] - H[j]) / 0.25 > SLOPE)
                {
                    break;
                }
                if (std::fabs(H[i] - h0) > MAXDZ)
                {
                    break;
                }
                F64 low = H[i];
                for (S32 b = i - d; b != c && std::abs(i - b) <= KERB_RUN; b -= d)
                {
                    if (has(H[b]))
                    {
                        low = std::min<F64>(low, H[b]);
                    }
                }
                if (H[i] - low > KERB)
                {
                    break;
                }
                prev = i;
            }
            (d < 0 ? left : right) = prev;
        }
    }

    // How far the linkset goes on past a deck edge at j, the way d.
    F64 beyond(const std::vector<F32>& H, S32 j, S32 d)
    {
        for (S32 k = d > 0 ? S32(H.size()) - 1 : 0; k != j; k -= d)
        {
            if (has(H[k]))
            {
                return std::abs(k - j) * DU;
            }
        }
        return 0.0;
    }

    struct KitEntry
    {
        bool done = false, ok = false;          // measured; and a kit
        Kit kit;
    };
    std::unordered_map<U64, KitEntry> sKits;
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

bool measureKit(const KitShape& shape, Kit& out, std::string* why)
{
    auto refuse = [why](const char* w)
    {
        if (why)
        {
            *why = w;
        }
        return false;
    };
    // The frame: the root's own axes, the one nearest the world's up as z (signed), the other two as x and y.
    const LLVector3 up = LLVector3::z_axis * ~shape.root_rot;
    S32 k = 0;
    for (S32 a = 1; a < 3; ++a)
    {
        if (std::fabs(up.mV[a]) > std::fabs(up.mV[k]))
        {
            k = a;
        }
    }
    const F32 sgn = up.mV[k] < 0.f ? -1.f : 1.f;
    const S32 A = k == 0 ? 1 : 0, B = k == 2 ? 1 : 2;

    std::vector<LLVector3> tris;            // (x, y, z) in that frame, three a triangle
    auto take = [&](const LLVector3& p) { tris.emplace_back(p.mV[A], p.mV[B], sgn * p.mV[k]); };
    for (const KitShapePrim& p : shape.prims)
    {
        if (!p.tris.empty())
        {
            for (const LLVector3& v : p.tris)
            {
                take(v);
            }
            continue;
        }
        LLPointer<LLVolume> volume = new LLVolume(p.params, LLVolumeLODGroup::getVolumeScaleFromDetail(LLVolumeLODGroup::NUM_LODS - 1));
        for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
        {
            const LLVolumeFace& face = volume->getVolumeFace(f);
            if (!face.mPositions || !face.mIndices)
            {
                continue;
            }
            for (S32 i = 0; i + 2 < face.mNumIndices; i += 3)
            {
                for (S32 c = 0; c < 3; ++c)
                {
                    const LLVector3 v(face.mPositions[face.mIndices[i + c]].getF32ptr());
                    take(LLVector3(v.mV[0] * p.scale.mV[0], v.mV[1] * p.scale.mV[1], v.mV[2] * p.scale.mV[2]) * p.rot + p.pos);
                }
            }
        }
    }
    if (tris.size() < 3)
    {
        return refuse("no geometry");
    }
    LLVector3 lo = tris[0], hi = tris[0];
    for (const LLVector3& v : tris)
    {
        for (S32 a = 0; a < 3; ++a)
        {
            lo.mV[a] = std::min(lo.mV[a], v.mV[a]);
            hi.mV[a] = std::max(hi.mV[a], v.mV[a]);
        }
    }

    // Which way it runs: the top envelope varies least along it.
    const S32 nx = S32(std::lround((hi.mV[0] - lo.mV[0]) / GRID)) + 1, ny = S32(std::lround((hi.mV[1] - lo.mV[1]) / GRID)) + 1;
    if (S64(nx) * ny > MAX_CELLS)
    {
        return refuse("too big for a road piece");
    }
    std::vector<F32> top(size_t(nx) * ny, NONE);
    auto raise = [&](S32 i, S32 j, F32 z)
    {
        F32& h = top[size_t(i) * ny + j];
        h = std::max(h, z);
    };
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
    {
        const LLVector3 &a = tris[t], &b = tris[t + 1], &c = tris[t + 2];
        for (const LLVector3* v : { &a, &b, &c })
        {
            raise(S32(std::lround((v->mV[0] - lo.mV[0]) / GRID)), S32(std::lround((v->mV[1] - lo.mV[1]) / GRID)), v->mV[2]);
        }
        const F64 det = F64(b.mV[0] - a.mV[0]) * (c.mV[1] - a.mV[1]) - F64(c.mV[0] - a.mV[0]) * (b.mV[1] - a.mV[1]);
        if (std::fabs(det) < 1e-9)
        {
            continue;                       // edge on from above
        }
        const S32 i0 = std::max(0, S32(std::ceil((std::min({ a.mV[0], b.mV[0], c.mV[0] }) - lo.mV[0]) / GRID)));
        const S32 i1 = std::min(nx - 1, S32(std::floor((std::max({ a.mV[0], b.mV[0], c.mV[0] }) - lo.mV[0]) / GRID)));
        const S32 j0 = std::max(0, S32(std::ceil((std::min({ a.mV[1], b.mV[1], c.mV[1] }) - lo.mV[1]) / GRID)));
        const S32 j1 = std::min(ny - 1, S32(std::floor((std::max({ a.mV[1], b.mV[1], c.mV[1] }) - lo.mV[1]) / GRID)));
        for (S32 i = i0; i <= i1; ++i)
        {
            const F64 x = lo.mV[0] + i * GRID - a.mV[0];
            for (S32 j = j0; j <= j1; ++j)
            {
                const F64 y = lo.mV[1] + j * GRID - a.mV[1];
                const F64 u = (x * (c.mV[1] - a.mV[1]) - y * (c.mV[0] - a.mV[0])) / det;
                const F64 v = (y * (b.mV[0] - a.mV[0]) - x * (b.mV[1] - a.mV[1])) / det;
                if (u >= -1e-6 && v >= -1e-6 && u + v <= 1 + 1e-6)
                {
                    raise(i, j, F32(a.mV[2] + u * (b.mV[2] - a.mV[2]) + v * (c.mV[2] - a.mV[2])));
                }
            }
        }
    }
    // the mean variance of the envelope along x (each line of it at one y), or along y
    auto meanVariance = [&](bool along_x)
    {
        F64 sum = 0;
        S32 lines = 0;
        for (S32 l = 0, nl = along_x ? ny : nx; l < nl; ++l)
        {
            F64 s = 0, s2 = 0;
            S32 m = 0;
            for (S32 e = 0, ne = along_x ? nx : ny; e < ne; ++e)
            {
                const F32 h = along_x ? top[size_t(e) * ny + l] : top[size_t(l) * ny + e];
                if (has(h))
                {
                    s += h;
                    s2 += F64(h) * h;
                    ++m;
                }
            }
            if (m)
            {
                sum += std::max(0.0, s2 / m - (s / m) * (s / m));
                ++lines;
            }
        }
        return lines ? sum / lines : 0.0;
    };
    const F64 var_x = meanVariance(true), var_y = meanVariance(false);
    const bool tie = std::max(var_x, var_y) < 1e-3 || std::fabs(var_x - var_y) < 0.1 * std::max(var_x, var_y);
    const bool along_x = tie ? hi.mV[0] - lo.mV[0] >= hi.mV[1] - lo.mV[1] : var_x < var_y;
    const S32 au = along_x ? 1 : 0, av = along_x ? 0 : 1;      // across and along, in the frame

    // Three slices across it, at 20, 50 and 80 % of its length, each walked out from its middle.
    const F64 umin = lo.mV[au], umax = hi.mV[au], vmin = lo.mV[av], vmax = hi.mV[av];
    const F64 u0 = umin - 1;
    const S32 n = S32((umax - umin + 2) / DU) + 1;
    std::vector<F64> from, to, kfrom, kto, widths, cambers;
    std::vector<F32> H(n);
    for (F64 f : { 0.2, 0.5, 0.8 })
    {
        const F64 v0 = vmin + f * (vmax - vmin);
        std::fill(H.begin(), H.end(), NONE);
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            const LLVector3* p[3] = { &tris[t], &tris[t + 1], &tris[t + 2] };
            F64 d[3], cu[6], cz[6];
            for (S32 c = 0; c < 3; ++c)
            {
                d[c] = p[c]->mV[av] - v0;
            }
            if ((d[0] > 0 && d[1] > 0 && d[2] > 0) || (d[0] < 0 && d[1] < 0 && d[2] < 0))
            {
                continue;
            }
            S32 m = 0;                      // where it meets the slice's plane
            for (S32 c = 0; c < 3; ++c)
            {
                const S32 e = (c + 1) % 3;
                if (d[c] == 0)
                {
                    cu[m] = p[c]->mV[au];
                    cz[m++] = p[c]->mV[2];
                }
                if ((d[c] < 0 && d[e] > 0) || (d[c] > 0 && d[e] < 0))
                {
                    const F64 s = d[c] / (d[c] - d[e]);
                    cu[m] = p[c]->mV[au] + s * (p[e]->mV[au] - p[c]->mV[au]);
                    cz[m++] = p[c]->mV[2] + s * (p[e]->mV[2] - p[c]->mV[2]);
                }
            }
            for (S32 a = 0; a < m; ++a)
            {
                for (S32 b = a + 1; b < m; ++b)
                {
                    sampleSegment(H, u0, cu[a], cz[a], cu[b], cz[b]);
                }
                if (m == 1)
                {
                    sampleSegment(H, u0, cu[a], cz[a], cu[a], cz[a]);
                }
            }
        }
        // start on the central plateau: the sample of the middle 40 % nearest its median height, and the middle
        std::vector<F64> mid;
        for (S32 i = S32(n * 0.3); i < S32(n * 0.7); ++i)
        {
            if (has(H[i]))
            {
                mid.push_back(H[i]);
            }
        }
        if (mid.empty())
        {
            continue;
        }
        const F64 med = median(mid);
        S32 c0 = -1;
        F64 best = 0;
        for (S32 i = S32(n * 0.3); i < S32(n * 0.7); ++i)
        {
            const F64 cost = std::fabs(H[i] - med) + 0.01 * std::abs(i - n / 2) * DU;
            if (has(H[i]) && (c0 < 0 || cost < best))
            {
                c0 = i;
                best = cost;
            }
        }
        S32 l = c0, r = c0;
        walkDeck(H, c0, l, r);
        F32 zlo = H[c0], zhi = H[c0];
        for (S32 i = l; i <= r; ++i)
        {
            if (has(H[i]))
            {
                zlo = std::min(zlo, H[i]);
                zhi = std::max(zhi, H[i]);
            }
        }
        from.push_back(u0 + l * DU);
        to.push_back(u0 + r * DU);
        kfrom.push_back(from.back() - beyond(H, l, -1));
        kto.push_back(to.back() + beyond(H, r, 1));
        widths.push_back((r - l) * DU);
        cambers.push_back(zhi - zlo);
    }
    if (widths.empty())
    {
        return refuse("no deck");
    }
    const F64 spread = *std::max_element(widths.begin(), widths.end()) - *std::min_element(widths.begin(), widths.end());
    Kit kit;
    kit.up = k;
    kit.up_sign = sgn < 0 ? -1 : 1;
    kit.across = along_x ? B : A;
    kit.along = along_x ? A : B;
    kit.deck_min = median(from);
    kit.deck_max = median(to);
    kit.kerb_min = median(kfrom);
    kit.kerb_max = median(kto);
    kit.crown = median(cambers);
    const F64 w = kit.deck_max - kit.deck_min;
    if (w < MIN_DECK)
    {
        return refuse("a strip, not a deck");
    }
    if (spread > VARIES)
    {
        return refuse("its deck changes width along it");
    }
    kit.confidence = spread < SAME_WIDTH && !tie ? "high" : "medium";
    const F64 centre = (kit.deck_min + kit.deck_max) / 2;
    if (w < ONE_LANE)
    {
        kit.lanes.push_back(centre);
    }
    else
    {
        const S32 each = std::max(1, S32(w / 2 / LANE_W));      // lanes each way, evenly over each half
        for (S32 i = each - 1; i >= 0; --i)
        {
            kit.lanes.push_back(centre - (i + 0.5) * w / 2 / each);
        }
        for (S32 i = 0; i < each; ++i)
        {
            kit.lanes.push_back(centre + (i + 0.5) * w / 2 / each);
        }
    }
    out = std::move(kit);
    return true;
}

U64 kitKey(U64 hash, const LLQuaternion& root_rot)
{
    const LLVector3 up = LLVector3::z_axis * ~root_rot;
    S32 k = 0;
    for (S32 a = 1; a < 3; ++a)
    {
        if (std::fabs(up.mV[a]) > std::fabs(up.mV[k]))
        {
            k = a;
        }
    }
    return hash ^ (0x9E3779B97F4A7C15ull * U64(k * 2 + (up.mV[k] < 0.f ? 1 : 2)));
}

const Kit* kitFor(U64 key)
{
    auto it = sKits.find(key);
    return it != sKits.end() && it->second.ok ? &it->second.kit : nullptr;
}

bool kitAsked(U64 key)
{
    return sKits.count(key) != 0;
}

bool kitDone(U64 key)
{
    auto it = sKits.find(key);
    return it != sKits.end() && it->second.done;
}

void kitAsk(U64 key)
{
    sKits.try_emplace(key);
}

void kitMeasured(U64 key, bool ok, const Kit& kit)
{
    KitEntry& e = sKits[key];
    e.done = true;
    e.ok = ok;
    e.kit = kit;
}

void forgetKits()
{
    sKits.clear();
}
}
