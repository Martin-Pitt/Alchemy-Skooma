/**
 * @file skroadmodel.cpp
 * @brief Spike: the road autopilot's road model (moved from skroaddriver.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#include "skroadmodel.h"
#include "skroadkits.h"
#include "skroadsigns.h"

#include "llsd.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llstring.h"
#include "skair.h"
#include "workqueue.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skroad
{
    namespace
    {
        F64 sHeldClock = -1.0;
    }

    constexpr F64 AHEAD_DEG = 35.0;
    constexpr F64 CROSSED_PIECE_M = 25.0;   // a linkset met both sides no wider across than this is crossed, not a street     // a way on whose piece lies within this of straight ahead carries the road on

    F64 clockSeconds()
    {
        return sHeldClock >= 0 ? sHeldClock : F64(LLTimer::getTotalSeconds().value());
    }

    void setClock(F64 seconds)
    {
        sHeldClock = seconds;
    }

    constexpr F64 DRIVEN_M = 16.0;
    constexpr F64 CHORD_M = 8.0;
    constexpr F64 MAX_TURN_DEG = 89.0;
    constexpr F64 REACH_M = 14.0;               // a cross-section's reach either side
    constexpr F64 SEAM_M = 0.05;                // narrower gaps between pieces are seams
    constexpr F64 BRIDGE_M = 1.2;               // with the surface known by texture: a line strip of its own
                                                // texture between lanes (the JP road's) is still the road
    constexpr F64 SEAM_NARROW = 0.6;            // a section narrower than this share of the last is a seam
    constexpr F64 SEAM_SLIDE_M = 0.75;          // how far along the road to look past it
    constexpr F64 MAX_STEP_TURN_DEG = 12.0;     // the walked line turns at most this much a step (2 m)
    constexpr F64 MAX_START_TURN_DEG = 30.0;    // and its first step this far from the car's heading
    constexpr F64 MAX_SHIFT_M = 0.6;            // the lane line may move this far sideways a step (2 m):
                                                // a checkpoint's apron or a lay-by does not pull it over
    constexpr F64 DIR_BASE_M = 3.0;             // the road's direction from sections this far behind and ahead
    constexpr F64 TWO_LANES_M = 2 * (2.0 + 2 * 0.5);   // a typical 2 m car with 0.5 m either side, twice
    constexpr F64 Z_REACH = 3.0;                // shapes this far above or below count at a cross-section
    const LLUUID LINDEN_MAINTENANCE("dc7b21cd-3c89-fcaa-31c8-25f9ffd224cd");     // Linden's "Maintenance" group: road
                                                                                // pieces on Linden land are set to it too

    F64 wrap(F64 a)
    {
        while (a > PI) a -= 2 * PI;
        while (a < -PI) a += 2 * PI;
        return a;
    }

    // How far ahead the lane path is laid out: what the controller rolls over at this speed, and at the least
    // SKRoadAutopilotLookAhead (to see junctions coming, and for the overlay); within the shapes' reach.
    constexpr F32 MAX_LOOK_AHEAD_M = 200.f;
    F64 lookAhead(F64 v)
    {
        const F32 least = host::lookAheadSetting();
        return std::max(v * (HORIZON + PATH_EVERY) + 3 * PATH_STEP, F64(llclamp(F32(least), 10.f, MAX_LOOK_AHEAD_M)));
    }

    // How far around the car road pieces are read (and the census asked for): the look-ahead and a margin.
    F32 shapeReach()
    {
        const F32 least = host::lookAheadSetting();
        return std::max(70.f, llclamp(F32(least), 10.f, MAX_LOOK_AHEAD_M) + 15.f);
    }
    Timer sCacheClock;

    // The driving surface's textures. A road object may carry its own verges and kerbs (Route 10's
    // segments: a 10 m paved root part between two 2.7 m grass parts, all named "SL - Route 10", all
    // at one height): only how they look tells them apart. Started from what is under the car, then
    // grown only by pieces under it that share a texture with the set (paving variants, never the grass).
    std::unordered_set<LLUUID> sDriveTex;
    U32 sDriveTexGen = 1;                       // moves whenever sDriveTex does: which parts match is worked out again
    void driveTexChanged()
    {
        ++sDriveTexGen;
    }
    S32 sOffSurfaceChecks = 0;                  // checks in a row the car was on road pieces none of the surface

    // The road ahead as linksets, chained end to end from the one under the car (see chain): while set,
    // cross-sections count only these linksets' pieces, so the line follows a curved linkset's own shape
    // exactly and never spills into a car park or a side road beside it.
    std::unordered_set<LLUUID> sAllowedRoots;
    // While set, cross-sections leave out a long piece lying across this way (a side road's strip at its mouth)
    // unless it is one of the two pieces named: the road's width is its own, not the side road's too.
    bool sWayOnly = false;
    F64 sWayX = 1, sWayY = 0;
    const PieceShapes* sWayKeep[2] = { nullptr, nullptr };
    std::vector<LLUUID> sChainRoots;
    std::vector<std::vector<S32>> sSiblings;    // a segment's halves alongside, once per set of segments (see siblings)
    std::vector<bool> sSiblingsKnown;
    // The road the car is on and the road ahead, as the last chain laid it: kept, so a moment on an awkward
    // spot (a crossing's cover, a seam, a sliver of an unlinked piece) does not lose it. A chain starts on it
    // and carries on along it, leaving it only where a junction's choice does.
    std::unordered_set<LLUUID> sRoute;
    F64 sRouteTime = -100.0;
    constexpr F64 ROUTE_KEEP_S = 3.0;           // how long the route is kept with no chain laying it again
    constexpr F64 ROUTE_BONUS = 0.5;
    constexpr F64 UNDECIDED_PENALTY = 0.8;      // a way on only road-shaped (a private pavement beside the road), under
                                                // any definite road going on            // a way on along the route, over the straightest
    // The line found is the road (Nexii's ruling: the road once found is kept, never deleted): kept until the car
    // has driven past it or is KEPT_OFF_M from it, however long, and a fresh walk that comes back shorter only
    // replaces it as far as it reaches, the kept line going on past its end. 2 s and gone, the line vanished for
    // a second or two each time a walk stopped short, and threw the steering off.
    constexpr F64 PATH_KEEP_S = 1e12;
    constexpr F64 KEPT_OFF_M = 10.0;
    constexpr F64 KEPT_AGREE_M = 1.5;           // a fresh line within this of the kept one at its end joins it
    Timer sSinceDriveTex;

    std::unordered_map<U64, PieceShapes> sShapeCache;   // by part
    std::deque<std::pair<F64, std::vector<Shape>>> sRetiredShapes;  // a changed part's old faces, while pointed at
    std::vector<const Shape*> sShapes;                   // in reach
    std::unordered_map<U64, std::vector<S32>> sHash;      // 16 m cells -> indices into sShapes
    constexpr F64 HASH_M = 16.0;
    Timer sSinceShapes;
    bool sShapesRead = false;
    std::deque<LLUUID> sToAsk;
    std::deque<LLUUID> sAskFirst;               // the road ahead's pieces with no name yet: asked before the rest
    std::unordered_set<LLUUID> sAskFirstSet;
    std::unordered_map<LLUUID, F64> sAsked;     // root -> when asked (or queued); asked again if no answer
    Timer sAskClock;
    Timer sSinceAsk;

    // Each linkset's root as the census last saw it (its root part's middle, global), and whose road it was found to
    // be (linksetAccess; kept once known).
    std::unordered_map<LLUUID, LLVector3d> sRootAt;
    std::unordered_map<LLUUID, S8> sAccess;

    U64 hashKey(S64 x, S64 y)
    {
        return (U64(U32(S32(x))) << 32) | U32(S32(y));
    }

    // Is this linkset road by its name or group, when the simulator has said? -1 unknown, 0 no, 1 yes. Settled
    // where nothing still to come (its owner's name or profile) could change the answer.
    S32 roadByName(const LLUUID& root, bool& settled)
    {
        settled = true;
        const host::Props* p = host::properties(root);
        if (!p)
        {
            return -1;      // until its properties come (roadVerdict hears of them)
        }
        if (p->mGroupId == LDPW)
        {
            host::learnLdpw(p->mOwnerId);      // only a member can set an object to LDPW
        }
        std::string n = p->mName;
        LLStringUtil::toLower(n);
        // a road word as a word of its own ("Roadside Kiosk" is no road)
        auto has_word = [&n](const char* word)
        {
            const size_t len = strlen(word);
            for (size_t at = n.find(word); at != std::string::npos; at = n.find(word, at + 1))
            {
                const bool start = at == 0 || !isalpha(static_cast<unsigned char>(n[at - 1]));
                const bool end = at + len >= n.size() || !isalpha(static_cast<unsigned char>(n[at + len]));
                // a word of its own, or its plural ("roads"); "roadside" is neither
                const bool plural = n.compare(at + len, 1, "s") == 0
                                    && (at + len + 1 >= n.size() || !isalpha(static_cast<unsigned char>(n[at + len + 1])));
                if (start && (end || plural))
                {
                    return true;
                }
            }
            return false;
        };
        bool road_word = false;
        for (const char* word : { "road", "route", "street", "driveway", "highway", "lane", "avenue" })
        {
            road_word |= has_word(word);
        }
        {
            std::string t = n;
            LLStringUtil::trim(t);
            const bool unnamed = t.empty() || t == "object" || t == "primitive";
            if (p->mGroupId == LDPW || p->mGroupId == LINDEN_MAINTENANCE)
            {
                // a road group's object is road when its name says so or it has none: the group's kiosks, rocks
                // and signs are set to it too ("RT8 Roadside Kiosk" in the middle of an X crossing was taken for
                // road, and the car drove into it)
                // road unless named as something else: the group's road pieces are often named for their make
                // ("R9 cobble - 3sections": Route 9 was lost entirely when a road word was required), while its
                // kiosks, rocks and signs say what they are
                bool other = false;
                for (const char* w : { "kiosk", "rock", "sign", "tree", "lamp", "rail", "fence", "bench", "shelter", "hut",
                                       "building", "bin", "booth", "post", "pole", "plant", "bush", "light", "barrier",
                                       "cone", "sculpt", "statue", "house", "shack", "wall", "stair", "bollard" })
                {
                    other |= has_word(w) || n.find(w) != std::string::npos;
                }
                (void)unnamed;
                return road_word || !other ? 1 : 0;
            }
        }
        const bool governor = host::lindenOwner(p->mOwnerId);
        for (const char* word : { "road", "route", "street", "driveway", "highway", "lane", "avenue" })
        {
            if (has_word(word))
            {
                // a road word is road outright only on a Linden's piece: a private kit's names say what it was
                // sold as, not where it lies ("[-BLUE SKY-] Cobblestone Road" laid as a town's pavement took the
                // line up the kerb); private roads come later, by their kits
                if (governor)
                {
                    return 1;
                }
                settled = false;
                return -1;
            }
        }
        LLStringUtil::trim(n);
        const bool default_name = n.empty() || n == "object" || n == "primitive";
        settled = governor;     // a Linden stays one; one not yet known to be may turn out to be
        if (default_name && governor)
        {
            return 1;       // the Linden roads: a Linden's or a mole's unnamed pieces (Governor Linden's, mostly)
        }
        if (default_name || governor)
        {
            return -1;      // says nothing either way (Governor Linden owns infohubs and bridges as well, and a
                            // default name anyone's): its shape and surface decide
        }
        return 0;
    }

    // roadByName, kept per linkset: worked out again when its properties come, and an unsettled answer at most
    // once a second.
    struct Verdict
    {
        S32 named = -1;
        bool settled = false;
        F64 when = 0;
    };
    std::unordered_map<LLUUID, Verdict> sVerdicts;
    S32 roadVerdict(const LLUUID& root)
    {
        [[maybe_unused]] static const bool props = host::watchProperties([](const LLUUID& id) { sVerdicts.erase(id); });
        const F64 now = sCacheClock.getElapsedTimeF64();
        auto [it, fresh] = sVerdicts.try_emplace(root);
        Verdict& v = it->second;
        if (fresh || (!v.settled && now - v.when > 1.0))
        {
            v.named = roadByName(root, v.settled);
            v.when = now;
        }
        return v.named;
    }

    // ---------------------------------------------------------------- where the road ends, by name

    // `word` in lower-case `n` as a word of its own
    bool wordIn(const std::string& n, const char* word)
    {
        const size_t len = strlen(word);
        for (size_t at = n.find(word); at != std::string::npos; at = n.find(word, at + 1))
        {
            const bool start = at == 0 || !isalpha(static_cast<unsigned char>(n[at - 1]));
            const bool end = at + len >= n.size() || !isalpha(static_cast<unsigned char>(n[at + len]));
            if (start && end)
            {
                return true;
            }
        }
        return false;
    }

    // A road piece named as the road's end: "RT12 road endcap", "end cap", "Road End" (Dreyfus, Lodestone). The
    // road stops at its far side.
    bool endPieceByName(const LLUUID& root)
    {
        const host::Props* p = host::properties(root);
        if (!p)
        {
            return false;
        }
        std::string n = p->mName;
        LLStringUtil::toLower(n);
        return wordIn(n, "end") || wordIn(n, "ends") || wordIn(n, "endcap") || wordIn(n, "end cap");
    }

    // What a linkset's faces say as a road sign, by texture (the sign library): a road closed or ending first, then a
    // speed limit or warning, then anything else it knows; null where none, or until the linkset is live.
    const Sign* signByTexture(const LLUUID& root)
    {
        static std::string path;
        static std::unordered_map<LLUUID, const Sign*> known;
        const std::string want = host::signLibraryPath();
        if (want != path)
        {
            path = want;
            known.clear();
            loadSignLibrary(path);
        }
        if (path.empty())
        {
            return nullptr;
        }
        if (auto it = known.find(root); it != known.end())
        {
            return it->second;
        }
        std::vector<LLUUID> textures;
        if (!host::signTextures(root, textures))
        {
            return nullptr;
        }
        const Sign* best = nullptr;
        auto rank = [](const Sign* s)
        {
            return !s ? 0 : s->what == SignClass::RoadClosed || s->what == SignClass::RoadEnds ? 3 : s->what == SignClass::SpeedLimit ? 2 : 1;
        };
        for (const LLUUID& t : textures)
        {
            const Sign* s = signFor(t);
            if (rank(s) > rank(best))
            {
                best = s;
            }
        }
        known[root] = best;
        return best;
    }

    // A sign saying the road ends or is closed where it stands ("Road Ends Sign", the LDPW's yellow diamond on a pier's end):
    // 0 none, 1 a sign, 2 one the Lindens put there (LDPW, a Linden's or a mole's), 3 one whose texture says so
    // (a Linden "Sign - Fly Lane" reads ROAD CLOSED). "Dead end" and "no through road"
    // signs stand where such a road begins, not where it ends: they are no end here (the deck's own end is).
    S32 endSignByName(const LLUUID& root)
    {
        if (const Sign* sign = signByTexture(root); sign && (sign->what == SignClass::RoadClosed || sign->what == SignClass::RoadEnds))
        {
            return 3;
        }
        const host::Props* p = host::properties(root);
        if (!p)
        {
            return 0;
        }
        std::string n = p->mName;
        LLStringUtil::toLower(n);
        // "Road Closed" (the LDPW's diamond beside a broken road) and a bare "closed" say the same: no way on past it
        const bool says = ((wordIn(n, "road") || wordIn(n, "street") || wordIn(n, "route")) && (wordIn(n, "ends") || wordIn(n, "end")))
                          || wordIn(n, "closed");
        if (!says || wordIn(n, "dead") || n.find("through") != std::string::npos)
        {
            return 0;
        }
        return p->mGroupId == LDPW || host::lindenOwner(p->mOwnerId) ? 2 : 1;
    }

    // What stands beside the road in the gather box and could be a sign: small, upright parts (gather).
    struct SignCandidate
    {
        LLUUID root;
        F64 x, y, z0, z1;
        F64 lx = 1, ly = 0;     // its linkset's long axis in plan: a rail's run, a sign panel's face (normal across it)
        bool oriented = false;  // that axis is plain (the plan at least twice as long as wide), not a post
    };
    std::vector<SignCandidate> sSignCandidates;
    constexpr F64 END_SIGN_SIZE_M = 4.0;        // a sign's plan at most this across
    constexpr F64 END_SIGN_BESIDE_M = 6.0;      // and this far off the deck's edge, or on it
    constexpr F64 END_SIGN_Z_M = 3.0;           // standing within this of the deck's height
    constexpr F64 END_SIGN_ACROSS_SIN = 0.5;    // a marker's long axis within 30 degrees of square to the road it ends
    constexpr F64 END_SIGN_MARGIN_M = 1.5;      // and on that road's own width, this past its edges

    U64 mix(U64 h)
    {
        h ^= h >> 30;
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 27;
        h *= 0x94D049BB133111EBULL;
        return h ^ (h >> 31);
    }

    // A corner as joins compare corners: to a centimetre across, five up (in the world; a mesh's own frame is
    // finer).
    U64 cornerKey(F64 x, F64 y, F64 z, F64 qxy = 100, F64 qz = 20)
    {
        return mix(mix(mix(U64(std::llround(x * qxy))) ^ U64(std::llround(y * qxy))) ^ U64(std::llround(z * qz)));
    }

    U64 edgeKey(U64 a, U64 b)
    {
        return mix(std::min(a, b) * 0x9E3779B97F4A7C15ULL ^ std::max(a, b));
    }

    // Its plan's bounds, and those of where inside() holds: its edges' lines moved out by inside()'s tolerance.
    void bound(Shape& s)
    {
        s.x0 = s.x1 = s.x[0];
        s.y0 = s.y1 = s.y[0];
        F64 area = 0;
        for (S32 k = 0; k < s.n; ++k)
        {
            s.x0 = std::min(s.x0, s.x[k]);
            s.x1 = std::max(s.x1, s.x[k]);
            s.y0 = std::min(s.y0, s.y[k]);
            s.y1 = std::max(s.y1, s.y[k]);
            area += s.x[k] * s.y[(k + 1) % s.n] - s.x[(k + 1) % s.n] * s.y[k];
        }
        // inside(): sign * (ex (y - y0) - ey (x - x0)) >= -0.05 for every edge, a line a x + b y = c moved out
        const F64 sign = area > 0 ? 1.0 : -1.0;
        F64 a[4], b[4], c[4];
        for (S32 k = 0; k < s.n; ++k)
        {
            const F64 ex = s.x[(k + 1) % s.n] - s.x[k], ey = s.y[(k + 1) % s.n] - s.y[k];
            a[k] = -sign * ey;
            b[k] = sign * ex;
            c[k] = a[k] * s.x[k] + b[k] * s.y[k] - 0.05;
        }
        // A triangle's lines moved out still make a triangle and a parallelogram's a parallelogram, cornered where
        // neighbouring lines cross; anything else (or a sliver with two edges in line) is tested wherever it is.
        bool loose = area == 0
                     || (s.n == 4 && (std::fabs(s.x[0] + s.x[2] - s.x[1] - s.x[3]) > 1e-6 || std::fabs(s.y[0] + s.y[2] - s.y[1] - s.y[3]) > 1e-6));
        s.tx0 = s.x0;
        s.tx1 = s.x1;
        s.ty0 = s.y0;
        s.ty1 = s.y1;
        for (S32 k = 0; k < s.n && !loose; ++k)
        {
            const S32 p = (k + s.n - 1) % s.n;
            const F64 det = a[p] * b[k] - a[k] * b[p];
            if (std::fabs(det) < 1e-12)
            {
                loose = true;
                break;
            }
            const F64 x = (c[p] * b[k] - c[k] * b[p]) / det, y = (a[p] * c[k] - a[k] * c[p]) / det;
            s.tx0 = std::min(s.tx0, x);
            s.tx1 = std::max(s.tx1, x);
            s.ty0 = std::min(s.ty0, y);
            s.ty1 = std::max(s.ty1, y);
        }
        if (loose)
        {
            // not a triangle or parallelogram: its plan box padded by inside()'s margin and more. Never infinite:
            // that made every pair of linksets near one overlap, and 40-80 k pairs were joined again every 0.5 s
            s.tx0 = s.x0 - 0.1;
            s.ty0 = s.y0 - 0.1;
            s.tx1 = s.x1 + 0.1;
            s.ty1 = s.y1 + 0.1;
        }
    }

    constexpr F64 SIDE_COS = 0.99939;           // cos 2 degrees: faces this near in direction,
    constexpr F64 SIDE_PLANE_M = 0.02;          // and this near the first one's plane, are one side of a part

    struct ByKey
    {
        bool operator()(const std::pair<U64, S32>& a, U64 k) const { return a.first < k; }
        bool operator()(U64 k, const std::pair<U64, S32>& a) const { return k < a.first; }
    };

    // A part's sides: its faces merged where they meet edge to edge in one plane (a cut prim's top, given as
    // hundreds of triangles, is one or a few), each side's outline its edges no other face of it shares, in
    // loops. Joins are found along these, a few edges a part, rather than along every triangle's.
    void sidesOf(const std::vector<Shape>& shapes, F64 qxy, F64 qz, F64 plane_tol, std::vector<SideEdge>& out, U64& faces_hash)
    {
        const S32 nf = S32(shapes.size());
        std::vector<U64> corner(size_t(nf) * 4);
        std::vector<std::pair<U64, S32>> faces;     // (face key, face): each face once (a mesh may repeat one)
        faces.reserve(nf);
        for (S32 f = 0; f < nf; ++f)
        {
            const Shape& s = shapes[f];
            U64 sorted[4];
            for (S32 k = 0; k < s.n; ++k)
            {
                corner[f * 4 + k] = sorted[k] = cornerKey(s.x[k], s.y[k], s.zv[k], qxy, qz);
            }
            std::sort(sorted, sorted + s.n);
            U64 h = U64(s.n);
            for (S32 k = 0; k < s.n; ++k)
            {
                h = mix(h ^ sorted[k]);
            }
            faces.push_back({ h, f });
        }
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), faces.end());
        faces_hash = 0;
        std::vector<std::array<F64, 4>> plane(nf);  // unit normal, offset
        std::vector<std::pair<U64, S32>> by_edge;   // (edge key, face)
        for (const auto& [h, f] : faces)
        {
            faces_hash += mix(h);
            const Shape& s = shapes[f];
            const F64 ax = s.x[1] - s.x[0], ay = s.y[1] - s.y[0], az = s.zv[1] - s.zv[0];
            const F64 bx = s.x[2] - s.x[0], by = s.y[2] - s.y[0], bz = s.zv[2] - s.zv[0];
            F64 nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            const F64 len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= std::max(len, 1e-12);
            ny /= std::max(len, 1e-12);
            nz /= std::max(len, 1e-12);
            plane[f] = { nx, ny, nz, nx * s.x[0] + ny * s.y[0] + nz * s.zv[0] };
            for (S32 k = 0; k < s.n; ++k)
            {
                by_edge.push_back({ edgeKey(corner[f * 4 + k], corner[f * 4 + (k + 1) % s.n]), f });
            }
        }
        std::sort(by_edge.begin(), by_edge.end());
        struct Bound
        {
            U64 key, ka, kb;
            F64 x0, y0, z0, x1, y1, z1;
        };
        std::vector<S32> side(nf, -1), stack, members;
        std::vector<Bound> edges;
        std::vector<std::pair<U64, S32>> at;        // (corner key, outline edge)
        std::vector<bool> used;
        out.clear();
        for (const auto& [h0, seed] : faces)
        {
            if (side[seed] >= 0)
            {
                continue;
            }
            side[seed] = seed;
            stack.assign(1, seed);
            members.assign(1, seed);
            const std::array<F64, 4>& p0 = plane[seed];
            while (!stack.empty())
            {
                const S32 f = stack.back();
                stack.pop_back();
                const Shape& s = shapes[f];
                for (S32 k = 0; k < s.n; ++k)
                {
                    const U64 key = edgeKey(corner[f * 4 + k], corner[f * 4 + (k + 1) % s.n]);
                    const auto [lo, hi] = std::equal_range(by_edge.begin(), by_edge.end(), key, ByKey{});
                    for (auto it = lo; it != hi; ++it)
                    {
                        const S32 g = it->second;
                        const std::array<F64, 4>& pg = plane[g];
                        if (side[g] >= 0 || pg[0] * p0[0] + pg[1] * p0[1] + pg[2] * p0[2] < SIDE_COS)
                        {
                            continue;
                        }
                        const Shape& t = shapes[g];
                        bool flat = true;
                        for (S32 c = 0; c < t.n && flat; ++c)
                        {
                            flat = std::fabs(p0[0] * t.x[c] + p0[1] * t.y[c] + p0[2] * t.zv[c] - p0[3]) <= plane_tol;
                        }
                        if (flat)
                        {
                            side[g] = seed;
                            stack.push_back(g);
                            members.push_back(g);
                        }
                    }
                }
            }
            // the side's outline: its edges used once in it
            edges.clear();
            for (S32 f : members)
            {
                const Shape& s = shapes[f];
                for (S32 k = 0; k < s.n; ++k)
                {
                    const S32 k1 = (k + 1) % s.n;
                    const U64 ka = corner[f * 4 + k], kb = corner[f * 4 + k1];
                    edges.push_back({ edgeKey(ka, kb), ka, kb, s.x[k], s.y[k], s.zv[k], s.x[k1], s.y[k1], s.zv[k1] });
                }
            }
            std::sort(edges.begin(), edges.end(), [](const Bound& a, const Bound& b) { return a.key < b.key; });
            size_t kept = 0;
            for (size_t i = 0; i < edges.size();)
            {
                size_t j = i + 1;
                while (j < edges.size() && edges[j].key == edges[i].key)
                {
                    ++j;
                }
                if (j - i == 1)
                {
                    edges[kept++] = edges[i];
                }
                i = j;
            }
            edges.resize(kept);
            // in loops, corner to corner
            at.clear();
            for (S32 i = 0; i < S32(edges.size()); ++i)
            {
                at.push_back({ edges[i].ka, i });
                at.push_back({ edges[i].kb, i });
            }
            std::sort(at.begin(), at.end());
            used.assign(edges.size(), false);
            for (size_t s0 = 0; s0 < edges.size(); ++s0)
            {
                if (used[s0])
                {
                    continue;
                }
                used[s0] = true;
                const Bound& e0 = edges[s0];
                out.push_back({ e0.x0, e0.y0, e0.z0, e0.x1, e0.y1, e0.z1, e0.key, true, p0[0], p0[1], p0[2] });
                U64 cur = e0.kb;
                for (;;)
                {
                    const auto [lo, hi] = std::equal_range(at.begin(), at.end(), cur, ByKey{});
                    auto next = std::find_if(lo, hi, [&used](const std::pair<U64, S32>& a) { return !used[a.second]; });
                    if (next == hi)
                    {
                        break;
                    }
                    used[next->second] = true;
                    const Bound& e = edges[next->second];
                    if (e.ka == cur)
                    {
                        out.push_back({ e.x0, e.y0, e.z0, e.x1, e.y1, e.z1, e.key, false, p0[0], p0[1], p0[2] });
                        cur = e.kb;
                    }
                    else
                    {
                        out.push_back({ e.x1, e.y1, e.z1, e.x0, e.y0, e.z0, e.key, false, p0[0], p0[1], p0[2] });
                        cur = e.ka;
                    }
                }
            }
        }
    }

    PieceShapes makeFaces(const skworld::Record& r);
    constexpr F32 THICK_SLAB_M = 3.f;       // the thickest box slab read at all (a bridge deck's 2 m slabs)
    bool straightRun(const std::vector<std::array<F64, 3>>& p);

    // The part's faces, order-free, each once: a part duplicating another has the same.
    U64 facesHash(const std::vector<Shape>& shapes)
    {
        std::vector<U64> keys;
        keys.reserve(shapes.size());
        for (const Shape& s : shapes)
        {
            U64 sorted[4];
            for (S32 k = 0; k < s.n; ++k)
            {
                sorted[k] = cornerKey(s.x[k], s.y[k], s.zv[k]);
            }
            std::sort(sorted, sorted + s.n);
            U64 h = U64(s.n);
            for (S32 k = 0; k < s.n; ++k)
            {
                h = mix(h ^ sorted[k]);
            }
            keys.push_back(h);
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        U64 out = 0;
        for (U64 h : keys)
        {
            out += mix(h);
        }
        return out;
    }

    // A mesh's (or a sculpt's) sides in its own frame, before the object's scale: every copy of an asset is the
    // same geometry, only scaled, turned and placed, so its faces are merged into sides once per asset and each
    // copy's outline is those sides placed (a road kit's piece laid a hundred times is worked out once). Kept for
    // the session; the table a road kit's catalogue will read.
    struct AssetSides
    {
        std::vector<SideEdge> outline;          // in the mesh's frame, every side whatever way it faces
    };
    std::unordered_map<U64, AssetSides> sAssetSides;
    constexpr F64 ASSET_Q = 1e4;                // the mesh's frame is the unit box: corners compared to 1/10000 of it
    constexpr F64 ASSET_PLANE = 0.002;          // and a side's faces within 1/500 of it of one plane

    // The part's outline from its asset's sides, placed as the object is; false where it has no asset (a prim,
    // a part not loaded), and its own faces are merged instead.
    bool assetOutline(const skworld::Record& r, std::vector<SideEdge>& out)
    {
        if (r.mShape != skworld::Shape::TRIANGLES || !r.mTriangles)
        {
            return false;
        }
        host::MeshPlacement object;
        if (!host::meshPlacement(r, object))
        {
            return false;
        }
        const U64 key = mix(mix(U64(std::hash<LLUUID>{}(object.sculpt_id))) ^ (U64(object.sculpt_type) << 8)
                            ^ (U64(object.physics_shape) << 16) ^ (U64(r.mProvenance) << 24));
        const LLVector3 pos = object.pos;
        const LLQuaternion rot = object.rot;
        const LLVector3 scale = object.scale;
        if (scale.mV[VX] < 1e-3f || scale.mV[VY] < 1e-3f || scale.mV[VZ] < 1e-3f)
        {
            return false;
        }
        auto [it, fresh] = sAssetSides.try_emplace(key);
        if (fresh)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - assetSides");
            // the faces in the mesh's own frame: unturned, unplaced, unscaled
            const LLQuaternion unrot = ~rot;
            std::vector<Shape> local;
            const std::vector<LLVector3>& t = *r.mTriangles;
            for (size_t i = 0; i + 2 < t.size(); i += 3)
            {
                Shape s;
                s.n = 3;
                for (S32 k = 0; k < 3; ++k)
                {
                    LLVector3 l = (t[i + k] - pos) * unrot;
                    s.x[k] = l.mV[VX] / scale.mV[VX];
                    s.y[k] = l.mV[VY] / scale.mV[VY];
                    s.zv[k] = l.mV[VZ] / scale.mV[VZ];
                }
                const F64 ax = s.x[1] - s.x[0], ay = s.y[1] - s.y[0], az = s.zv[1] - s.zv[0];
                const F64 bx = s.x[2] - s.x[0], by = s.y[2] - s.y[0], bz = s.zv[2] - s.zv[0];
                const F64 nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                if (nx * nx + ny * ny + nz * nz < 1e-14)
                {
                    continue;
                }
                local.push_back(s);
            }
            U64 faces = 0;
            sidesOf(local, ASSET_Q, ASSET_Q, ASSET_PLANE, it->second.outline, faces);
        }
        // placed: scaled, turned, moved; a side's normal by the inverse scale. Its sides facing up or down (to
        // 60 degrees, as the faces are kept) are its outline, the walls' are not.
        const F64 ox = r.mOrigin.mdV[VX], oy = r.mOrigin.mdV[VY], oz = r.mOrigin.mdV[VZ];
        out.clear();
        bool start = true;
        for (const SideEdge& e : it->second.outline)
        {
            start = start || e.loop_start;
            LLVector3 n(F32(e.nx / scale.mV[VX]), F32(e.ny / scale.mV[VY]), F32(e.nz / scale.mV[VZ]));
            n = n * rot;
            const F32 len = n.normVec();
            if (len < 1e-6f || std::fabs(n.mV[VZ]) < 0.5f)
            {
                continue;
            }
            const LLVector3 a = pos + LLVector3(F32(e.x0) * scale.mV[VX], F32(e.y0) * scale.mV[VY], F32(e.z0) * scale.mV[VZ]) * rot;
            const LLVector3 b = pos + LLVector3(F32(e.x1) * scale.mV[VX], F32(e.y1) * scale.mV[VY], F32(e.z1) * scale.mV[VZ]) * rot;
            SideEdge w{ ox + a.mV[VX], oy + a.mV[VY], oz + a.mV[VZ], ox + b.mV[VX], oy + b.mV[VY], oz + b.mV[VZ], 0, start, n.mV[VX], n.mV[VY],
                        n.mV[VZ] };
            w.key = edgeKey(cornerKey(w.x0, w.y0, w.z0), cornerKey(w.x1, w.y1, w.z1));
            out.push_back(w);
            start = false;
        }
        return true;
    }

    // The part's top as a road piece: its upward sides' outline in straight runs, each with the way out across
    // it, and its frame (a box's own axes; otherwise the outline's long and short directions).
    void topOf(PieceShapes& ps, const skworld::Record& r)
    {
        ps.top.clear();
        ps.box = r.mShape == skworld::Shape::BOX && r.mProvenance == skworld::Provenance::EXACT && r.mCollides;
        std::vector<std::array<F64, 3>> run;
        std::vector<TopEdge> loop;
        auto flush = [&]()
        {
            if (run.size() >= 2)
            {
                loop.push_back({ run.front()[0], run.front()[1], run.front()[2], run.back()[0], run.back()[1], run.back()[2], 0, 0 });
            }
            run.clear();
        };
        auto close = [&]()
        {
            flush();
            // the way out of each edge: right of it on a loop running anticlockwise
            F64 area = 0;
            for (const TopEdge& e : loop)
            {
                area += e.x0 * e.y1 - e.x1 * e.y0;
            }
            for (TopEdge& e : loop)
            {
                const F64 L = std::hypot(e.x1 - e.x0, e.y1 - e.y0);
                if (L > 1e-6)
                {
                    const F64 s = area > 0 ? 1.0 : -1.0;
                    e.ox = s * (e.y1 - e.y0) / L;
                    e.oy = -s * (e.x1 - e.x0) / L;
                    ps.top.push_back(e);
                }
            }
            loop.clear();
        };
        for (const SideEdge& e : ps.outline)
        {
            if (e.loop_start)
            {
                close();
            }
            if (e.nz <= 0)
            {
                flush();
                continue;       // a bottom's outline: not where it is driven on
            }
            if (!run.empty())
            {
                run.push_back({ e.x1, e.y1, e.z1 });
                if (straightRun(run))
                {
                    continue;
                }
                run.pop_back();
                flush();
            }
            run.push_back({ e.x0, e.y0, e.z0 });
            run.push_back({ e.x1, e.y1, e.z1 });
        }
        close();
        // the frame: a box's top's own edges; otherwise the outline's corners' long direction
        F64 mx = 0, my = 0, mz = 0, n = 0;
        for (const TopEdge& e : ps.top)
        {
            mx += e.x0 + e.x1;
            my += e.y0 + e.y1;
            mz += e.z0 + e.z1;
            n += 2;
        }
        if (n == 0)
        {
            return;
        }
        mx /= n;
        my /= n;
        ps.zt = mz / n;
        F64 ang;
        if (ps.box && ps.shapes.size() == 1 && ps.shapes[0].n == 4)
        {
            const Shape& q = ps.shapes[0];
            const F64 a = std::hypot(q.x[1] - q.x[0], q.y[1] - q.y[0]), b = std::hypot(q.x[2] - q.x[1], q.y[2] - q.y[1]);
            ang = a >= b ? std::atan2(q.y[1] - q.y[0], q.x[1] - q.x[0]) : std::atan2(q.y[2] - q.y[1], q.x[2] - q.x[1]);
        }
        else
        {
            F64 sxx = 0, syy = 0, sxy = 0;
            for (const TopEdge& e : ps.top)
            {
                for (const auto& [x, y] : { std::pair<F64, F64>{ e.x0, e.y0 }, std::pair<F64, F64>{ e.x1, e.y1 } })
                {
                    sxx += (x - mx) * (x - mx);
                    syy += (y - my) * (y - my);
                    sxy += (x - mx) * (y - my);
                }
            }
            ang = 0.5 * std::atan2(2 * sxy, sxx - syy);
        }
        ps.ux = std::cos(ang);
        ps.uy = std::sin(ang);
        F64 ulo = 1e18, uhi = -1e18, vlo = 1e18, vhi = -1e18;
        for (const TopEdge& e : ps.top)
        {
            for (const auto& [x, y] : { std::pair<F64, F64>{ e.x0, e.y0 }, std::pair<F64, F64>{ e.x1, e.y1 } })
            {
                const F64 u = (x - mx) * ps.ux + (y - my) * ps.uy, v = -(x - mx) * ps.uy + (y - my) * ps.ux;
                ulo = std::min(ulo, u);
                uhi = std::max(uhi, u);
                vlo = std::min(vlo, v);
                vhi = std::max(vhi, v);
            }
        }
        ps.lu = uhi - ulo;
        ps.lv = vhi - vlo;
        const F64 mu = (ulo + uhi) / 2, mv = (vlo + vhi) / 2;
        ps.cx = mx + mu * ps.ux - mv * ps.uy;
        ps.cy = my + mu * ps.uy + mv * ps.ux;
        ps.bx0 = ps.by0 = 1e18;
        ps.bx1 = ps.by1 = -1e18;
        for (const Shape& s : ps.shapes)
        {
            ps.bx0 = std::min(ps.bx0, s.x0);
            ps.by0 = std::min(ps.by0, s.y0);
            ps.bx1 = std::max(ps.bx1, s.x1);
            ps.by1 = std::max(ps.by1, s.y1);
        }
    }

    // A part's top faces, their bounds, its sides and its top as a road piece: once per part and signature
    // (sShapeCache).
    PieceShapes makeShapes(const skworld::Record& r)
    {
        PieceShapes ps = makeFaces(r);
        for (Shape& s : ps.shapes)
        {
            bound(s);
        }
        ps.faces = facesHash(ps.shapes);
        if (!assetOutline(r, ps.outline))
        {
            U64 faces = 0;
            sidesOf(ps.shapes, 100, 20, SIDE_PLANE_M, ps.outline, faces);
        }
        topOf(ps, r);
        return ps;
    }
    // A piece's shape and linkset, exactly: any change to a corner, however small, is a change.
    U64 pieceVersion(const PieceShapes& ps)
    {
        U64 h = mix(std::hash<LLUUID>{}(ps.root));
        auto add = [&h](F64 v) { U64 b; std::memcpy(&b, &v, sizeof b); h = mix(h ^ b); };
        for (const Shape& s : ps.shapes)
        {
            for (S32 k = 0; k < s.n; ++k)
            {
                add(s.x[k]);
                add(s.y[k]);
                add(s.zv[k]);
            }
        }
        for (const TopEdge& e : ps.top)
        {
            add(e.x0); add(e.y0); add(e.z0); add(e.x1); add(e.y1); add(e.z1);
        }
        return h;
    }
    // What a cache built from a piece is keyed on: the piece and its version.
    U64 pieceKey(const PieceShapes& ps)
    {
        return ps.id ^ mix(ps.version);
    }
    PieceShapes makeFaces(const skworld::Record& r)
    {
        PieceShapes ps;
        ps.signature = r.mSignature;
        ps.root = r.mRoot;
        ps.part = r.mPart;
        if (r.mDynamic)
        {
            return ps;
        }
        const F64 ox = r.mOrigin.mdV[VX], oy = r.mOrigin.mdV[VY], oz = r.mOrigin.mdV[VZ];
        if (r.mShape == skworld::Shape::TRIANGLES && r.mTriangles)
        {
            const std::vector<LLVector3>& t = *r.mTriangles;
            for (size_t i = 0; i + 2 < t.size(); i += 3)
            {
                const LLVector3 n = (t[i + 1] - t[i]) % (t[i + 2] - t[i]);
                const F32 len = n.magVec();
                if (len < 1e-6f || std::fabs(n.mV[VZ] / len) < 0.5f)
                {
                    continue;       // a wall or a kerb face, not something to drive on (roads to 60 degrees)
                }
                Shape s;
                s.n = 3;
                for (S32 k = 0; k < 3; ++k)
                {
                    s.x[k] = ox + t[i + k].mV[VX];
                    s.y[k] = oy + t[i + k].mV[VY];
                    s.zv[k] = oz + t[i + k].mV[VZ];
                }
                s.z = oz + (t[i].mV[VZ] + t[i + 1].mV[VZ] + t[i + 2].mV[VZ]) / 3.0;
                ps.shapes.push_back(s);
            }
        }
        else if (r.mShape == skworld::Shape::BOX)
        {
            S32 o[3] = { 0, 1, 2 };
            std::sort(o, o + 3, [&r](S32 a, S32 b) { return r.mHalf.mV[a] < r.mHalf.mV[b]; });
            if (std::fabs(r.mAxes[o[0]].mV[VZ]) < 0.5f || 2.f * r.mHalf.mV[o[0]] > THICK_SLAB_M)
            {
                return ps;      // stands up, or a block rather than a slab
            }
            ps.thick = 2.f * r.mHalf.mV[o[0]] > 1.5f;     // road only where it carries a road on (Pop's bridge deck)
            const S32 u = o[1], v = o[2], w = o[0];
            const F64 up = r.mAxes[w].mV[VZ] > 0 ? 1.0 : -1.0;     // the top face's side along the thin axis
            Shape s;
            s.n = 4;
            const F64 su[4] = { -1, 1, 1, -1 }, sv[4] = { -1, -1, 1, 1 };
            for (S32 k = 0; k < 4; ++k)
            {
                s.x[k] = ox + r.mCenter.mV[VX] + su[k] * r.mHalf.mV[u] * r.mAxes[u].mV[VX] + sv[k] * r.mHalf.mV[v] * r.mAxes[v].mV[VX];
                s.y[k] = oy + r.mCenter.mV[VY] + su[k] * r.mHalf.mV[u] * r.mAxes[u].mV[VY] + sv[k] * r.mHalf.mV[v] * r.mAxes[v].mV[VY];
                s.zv[k] = oz + r.mCenter.mV[VZ] + up * r.mHalf.mV[w] * r.mAxes[w].mV[VZ]
                          + su[k] * r.mHalf.mV[u] * r.mAxes[u].mV[VZ] + sv[k] * r.mHalf.mV[v] * r.mAxes[v].mV[VZ];
                s.z += s.zv[k] / 4;
            }
            // wound anticlockwise from above, as a top is: a box longer along its x than its y came out wound the
            // other way, its top taken for a bottom, and the box was no road part (Behrii's deck slabs: the walk
            // ran on the 3 m kerb strips beside them, the road read 2-3 m wide)
            if ((s.x[1] - s.x[0]) * (s.y[2] - s.y[0]) - (s.y[1] - s.y[0]) * (s.x[2] - s.x[0]) < 0)
            {
                std::swap(s.x[1], s.x[3]);
                std::swap(s.y[1], s.y[3]);
                std::swap(s.zv[1], s.zv[3]);
            }
            ps.shapes.push_back(s);
        }
        return ps;
    }

    // The road pieces about the car by linkset, as the last gather found them.
    struct Gathered
    {
        LLUUID root;
        S32 named = -1;                         // roadVerdict; 0: not road, its pieces not read
        std::vector<PieceShapes*> pieces;       // its parts with top faces, by id
        U64 key = 0;                            // what its segment is made from (settleSegments)
        std::vector<PieceShapes*> doubtful;     // flat parts road only where they carry a road on (carryOn)
        bool carried = false;                   // some of them do: road whatever its name says
    };
    std::vector<Gathered> sGathered;
    bool roadOf(const Gathered& g)
    {
        return g.named != 0 || g.carried;
    }
    bool definiteOf(const Gathered& g)
    {
        return g.named == 1 || g.carried;
    }
    void carryOn(F64 now);
    void settleSegments(bool force);
    bool sJoinsIncomplete = false;              // some joins were left for want of time: the next frame goes on

    // The pieces are gathered from the census past the reach, and again only once the car has gone some way
    // from where they were gathered, or the census says pieces in that box came, went or changed.
    constexpr F64 GATHER_MARGIN_M = 30.0;
    constexpr F64 GATHER_MOVE_M = 15.0;
    constexpr F64 GATHER_Z_M = 30.0;            // up and down (they counted to 25 m from the car), again past 5 m
    constexpr F64 GATHER_DIRTY_S = 2.0;
    constexpr F64 GATHER_STALE_S = 10.0;        // and at the least this often while the census changes
    LLVector3d sGatherAt;
    F64 sGatherReach = -1;
    bool sGatherDirty = true;
    U64 sGatherStamp = 0;
    Timer sSinceGather;
    std::weak_ptr<const CensusView> sDeltaSeen;   // the last scan whose changes were looked at

    bool inGatherBox(const skworld::Record& rec, const LLVector3d& at, F64 reach)
    {
        const F64 r = reach + GATHER_MARGIN_M;
        return !(rec.mMax.mdV[VX] < at.mdV[VX] - r || rec.mMin.mdV[VX] > at.mdV[VX] + r || rec.mMax.mdV[VY] < at.mdV[VY] - r
                 || rec.mMin.mdV[VY] > at.mdV[VY] + r || rec.mMax.mdV[VZ] < at.mdV[VZ] - GATHER_Z_M
                 || rec.mMin.mdV[VZ] > at.mdV[VZ] + GATHER_Z_M);
    }

    constexpr size_t CARRY_TOP_EDGES = 32;  // a deck's top is simple: what has more is a floor, a terrace (and slow to join)

    // Flat and wide enough to drive on, however it is named: a deck, a slab (what carryOn may find carries a road on).
    bool flatSlab(const skworld::Record& rec)
    {
        const F64 dx = rec.mMax.mdV[VX] - rec.mMin.mdV[VX], dy = rec.mMax.mdV[VY] - rec.mMin.mdV[VY];
        const F64 dz = rec.mMax.mdV[VZ] - rec.mMin.mdV[VZ];
        if (std::min(dx, dy) < 2.0 || std::max(dx, dy) < 4.0)
        {
            return false;
        }
        if (rec.mShape == skworld::Shape::BOX)
        {
            const F32 thin = 2.f * std::min({ rec.mHalf.mV[0], rec.mHalf.mV[1], rec.mHalf.mV[2] });
            return thin <= THICK_SLAB_M && dz <= THICK_SLAB_M + 0.2 * std::max(dx, dy);
        }
        return rec.mShape == skworld::Shape::TRIANGLES && dz <= THICK_SLAB_M;
    }

    void askLater(const LLUUID& root)
    {
        if (sAsked.try_emplace(root, -1.0).second)
        {
            sToAsk.push_back(root);     // ask what it is; road-shaped meanwhile
        }
    }

    // The road pieces in the box about the car: one record a part, by linkset, with their shapes.
    void gather(const CensusView& snap, const LLVector3d& car, F64 reach)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - gather");
        sGatherAt = car;
        sGatherReach = reach;
        sGatherStamp = snap.mStamp;
        sGatherDirty = false;
        sSinceGather.reset();
        const F64 now = sCacheClock.getElapsedTimeF64();
        // One record a part: a part can have several (its physics shape beside its visible one) carrying the
        // same faces, five to a face on Route 9's kit, which multiplied every shape, segment, join and line.
        // The one the car collides with, where there is one.
        static std::vector<const skworld::Record*> in_box;
        static std::unordered_map<LLUUID, size_t> one;
        static std::unordered_map<LLUUID, S32> roots;   // -> its index in sGathered
        in_box.clear();
        one.clear();
        roots.clear();
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - gather box");
            for (const skworld::RecordPtr& rec : snap.mRecords)
            {
                if (rec && rec->mPart == rec->mRoot)
                {
                    // the root, wherever it lies: its parcel says whose road its pieces are
                    sRootAt[rec->mRoot] = rec->mShape == skworld::Shape::TRIANGLES ? (rec->mMin + rec->mMax) * 0.5
                                                                                    : rec->mOrigin + LLVector3d(rec->mCenter);
                }
                if (!rec || rec->mDynamic || !inGatherBox(*rec, car, reach))
                {
                    continue;
                }
                auto [it, fresh] = one.try_emplace(rec->mPart, in_box.size());
                if (fresh)
                {
                    in_box.push_back(rec.get());
                }
                else if (!in_box[it->second]->mCollides && rec->mCollides)
                {
                    in_box[it->second] = rec.get();
                }
            }
        }
        // the small upright things that may be signs saying the road ends (endSignByName); a phantom one is asked
        // about here, as the shapes below ask only of what collides
        sSignCandidates.clear();
        for (const skworld::Record* rec : in_box)
        {
            const F64 w = rec->mMax.mdV[VX] - rec->mMin.mdV[VX], d = rec->mMax.mdV[VY] - rec->mMin.mdV[VY];
            const F64 h = rec->mMax.mdV[VZ] - rec->mMin.mdV[VZ];
            if (std::max(w, d) > END_SIGN_SIZE_M || h < 0.5 || h > 2.0 * END_SIGN_SIZE_M)
            {
                continue;
            }
            sSignCandidates.push_back({ rec->mRoot, 0.5 * (rec->mMin.mdV[VX] + rec->mMax.mdV[VX]), 0.5 * (rec->mMin.mdV[VY] + rec->mMax.mdV[VY]),
                                        rec->mMin.mdV[VZ], rec->mMax.mdV[VZ] });
            if (!rec->mCollides && h >= 0.8)
            {
                askLater(rec->mRoot);
            }
        }
        // each candidate linkset's long axis in plan, from all its parts in the box: a rail laid along the road's edge
        // is no end of that road, one laid across it is
        if (!sSignCandidates.empty())
        {
            struct Moments { F64 n = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0; };
            std::unordered_map<LLUUID, Moments> moments;
            for (const SignCandidate& c : sSignCandidates)
            {
                moments.try_emplace(c.root);
            }
            for (const skworld::Record* rec : in_box)
            {
                auto it = moments.find(rec->mRoot);
                if (it == moments.end())
                {
                    continue;
                }
                Moments& m = it->second;
                auto add = [&m](F64 x, F64 y)
                {
                    m.n += 1;
                    m.sx += x;
                    m.sy += y;
                    m.sxx += x * x;
                    m.syy += y * y;
                    m.sxy += x * y;
                };
                const F64 ox = rec->mOrigin.mdV[VX], oy = rec->mOrigin.mdV[VY];
                if (rec->mShape == skworld::Shape::TRIANGLES && rec->mTriangles)
                {
                    for (const LLVector3& v : *rec->mTriangles)
                    {
                        add(ox + v.mV[VX], oy + v.mV[VY]);
                    }
                }
                else
                {
                    for (S32 k = 0; k < 8; ++k)
                    {
                        LLVector3 v = rec->mCenter;
                        for (S32 a = 0; a < 3; ++a)
                        {
                            v += rec->mAxes[a] * (rec->mHalf.mV[a] * ((k >> a) & 1 ? 1.f : -1.f));
                        }
                        add(ox + v.mV[VX], oy + v.mV[VY]);
                    }
                }
            }
            for (SignCandidate& c : sSignCandidates)
            {
                const Moments& m = moments[c.root];
                if (m.n < 2)
                {
                    continue;
                }
                const F64 mx = m.sx / m.n, my = m.sy / m.n;
                const F64 cxx = m.sxx / m.n - mx * mx, cyy = m.syy / m.n - my * my, cxy = m.sxy / m.n - mx * my;
                const F64 half = 0.5 * (cxx + cyy), disc = std::sqrt(std::max(0.0, 0.25 * (cxx - cyy) * (cxx - cyy) + cxy * cxy));
                const F64 big = half + disc, small = std::max(half - disc, 0.0);
                const F64 ang = 0.5 * std::atan2(2.0 * cxy, cxx - cyy);
                c.lx = std::cos(ang);
                c.ly = std::sin(ang);
                c.oriented = big > 4.0 * small;     // variance ratio 4: twice as long as wide
            }
        }
        sGathered.clear();
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - gather shapes");
            for (const skworld::Record* rec : in_box)
            {
                if (!rec->mCollides)
                {
                    continue;       // phantom: nothing drives on it (a stone arch's top passed as a branch)
                }
                auto [root, root_fresh] = roots.try_emplace(rec->mRoot, S32(sGathered.size()));
                if (root_fresh)
                {
                    sGathered.push_back({ rec->mRoot, roadVerdict(rec->mRoot), {}, 0 });
                    if (sGathered.back().named < 0)
                    {
                        askLater(rec->mRoot);
                    }
                }
                Gathered& g = sGathered[root->second];
                if (g.named == 0 && !flatSlab(*rec))
                {
                    continue;       // a named thing that is not road (a car park, a floor, a roof)
                }
                // the part is the piece; its shape and linkset are what it carries, made again when the census's
                // signature moves and kept where they came out the same (the signature moves without the shape)
                const U64 id = mix(std::hash<LLUUID>{}(rec->mPart));
                auto [it, fresh] = sShapeCache.try_emplace(id);
                PieceShapes& piece = it->second;
                if (fresh || piece.signature != rec->mSignature || piece.root != rec->mRoot)
                {
                    PieceShapes made = makeShapes(*rec);
                    made.id = id;
                    made.version = pieceVersion(made);
                    if (fresh || made.version != piece.version)
                    {
                        if (!fresh)
                        {
                            sRetiredShapes.emplace_back(now, std::move(piece.shapes));
                            made.textures = std::move(piece.textures);
                            made.textures_known = piece.textures_known;
                            made.top_tex = piece.top_tex;
                            made.top_known = piece.top_known;
                        }
                        piece = std::move(made);
                        for (Shape& sh : piece.shapes)
                        {
                            sh.piece = &piece;
                        }
                    }
                    piece.signature = rec->mSignature;
                }
                piece.used = now;
                host::learnTextures(piece);
                if (g.named == 0 || piece.thick)
                {
                    if (!piece.top.empty() && piece.top.size() <= CARRY_TOP_EDGES)
                    {
                        g.doubtful.push_back(&piece);
                    }
                }
                else if (!piece.shapes.empty())
                {
                    g.pieces.push_back(&piece);
                }
            }
        }
        carryOn(now);
        for (Gathered& g : sGathered)
        {
            std::sort(g.pieces.begin(), g.pieces.end(), [](const PieceShapes* a, const PieceShapes* b) { return a->id < b->id; });
        }
    }

    // sShapes and sHash from the road pieces gathered.
    void indexShapes()
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - indexShapes");
        sShapes.clear();
        if (sHash.size() > 8192)
        {
            sHash.clear();
        }
        for (auto& cell : sHash)
        {
            cell.second.clear();        // the cells kept, their lists' storage too
        }
        for (const Gathered& g : sGathered)
        {
            if (!roadOf(g))
            {
                continue;
            }
            for (const PieceShapes* piece : g.pieces)
            {
                for (const Shape& s : piece->shapes)
                {
                    const S32 idx = S32(sShapes.size());
                    sShapes.push_back(&s);
                    for (S64 gx = S64(std::floor(s.x0 / HASH_M)); gx <= S64(std::floor(s.x1 / HASH_M)); ++gx)
                    {
                        for (S64 gy = S64(std::floor(s.y0 / HASH_M)); gy <= S64(std::floor(s.y1 / HASH_M)); ++gy)
                        {
                            sHash[hashKey(gx, gy)].push_back(idx);
                        }
                    }
                }
            }
        }
    }

    // The shapes of the road pieces within reach of the car: looked at every half second, gathered again only
    // when the pieces about the car may have changed.
    void forgetRoute();
    void forgetKitPlaces();

    void forgetRoad()
    {
        forgetRoute();
        sShapesRead = false;
        sGatherDirty = true;
        sGatherReach = -1;
        sGatherAt = LLVector3d(1e9, 1e9, 0);
        sPath.clear();
        sPathTime = -100.0;
        sRoute.clear();
        sDriveTex.clear();
        forgetKitPlaces();
        sDriven.clear();
        sHaveLastLine = false;
        sView = View();
        sAccess.clear();
    }

    void refreshShapes(const LLVector3d& car)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - refreshShapes");
        CensusPtr snap = host::census();
        if (snap && (sDeltaSeen.owner_before(snap) || snap.owner_before(sDeltaSeen)))
        {
            // every scan's changes, so none is missed between gathers
            sDeltaSeen = snap;
            for (const std::vector<skworld::RecordPtr>* list : { &snap->mAdded, &snap->mRemoved, &snap->mChanged })
            {
                for (size_t i = 0; i < list->size() && !sGatherDirty; ++i)
                {
                    const skworld::RecordPtr& rec = (*list)[i];
                    sGatherDirty = rec && !rec->mDynamic && inGatherBox(*rec, sGatherAt, sGatherReach);
                }
            }
        }
        if (sShapesRead && sSinceShapes.getElapsedTimeF64() < 0.5 && !sJoinsIncomplete)
        {
            return;     // (joins left over for want of time go on every frame until done)
        }
        sShapesRead = true;
        sSinceShapes.reset();
        if (!snap)
        {
            sGathered.clear();
            sGatherDirty = true;
            indexShapes();
            settleSegments(true);
            return;
        }
        const F64 reach = shapeReach();
        const F64 moved = std::hypot(car.mdV[VX] - sGatherAt.mdV[VX], car.mdV[VY] - sGatherAt.mdV[VY]);
        bool index = false;
        // a change the census saw about the car: gathered again no sooner than GATHER_DIRTY_S after the last (in a
        // town some object in reach changes on nearly every scan: 30 gathers in 20 s standing still)
        const bool dirty = sGatherDirty && sSinceGather.getElapsedTimeF64() > GATHER_DIRTY_S;
        if (dirty || reach != sGatherReach || moved > GATHER_MOVE_M || std::fabs(car.mdV[VZ] - sGatherAt.mdV[VZ]) > 5.0
            || (snap->mStamp != sGatherStamp && sSinceGather.getElapsedTimeF64() > GATHER_STALE_S))
        {
            gather(*snap, car, reach);
            index = true;
        }
        else
        {
            // what is road may have been said since, and parts' textures come with their objects
            bool regather = false;
            for (Gathered& g : sGathered)
            {
                const S32 named = roadVerdict(g.root);
                if (named != g.named)
                {
                    const bool was_road = g.named != 0;
                    g.named = named;
                    if (!was_road || g.carried || !g.doubtful.empty())     // what carries a road on is found again
                    {
                        regather = true;    // its pieces were never read
                        continue;
                    }
                    if (named == 0)
                    {
                        index = true;
                        g.pieces.clear();   // not road: its pieces are let go (evictCaches)
                    }
                }
                if (named < 0)
                {
                    askLater(g.root);
                }
                if (named != 0)
                {
                    for (PieceShapes* p : g.pieces)
                    {
                        host::learnTextures(*p);
                    }
                }
            }
            if (regather)
            {
                gather(*snap, car, reach);
                index = true;
            }
        }
        if (index)
        {
            indexShapes();
        }
        settleSegments(index);
        host::sortAsks(car);
    }

    // Linksets none of whose parts share the driving surface's textures (a bridge, a junction piece or a
    // side road in another surface): taken whole when the chain leads onto them, since texture is a strong
    // sign of the road along a stretch but changes at bridges and junctions.
    std::unordered_set<LLUUID> sOpenRoots;
    // Linksets only road-shaped and laid out as a wide square (a car park, a plaza): no part of the road.
    std::unordered_set<LLUUID> sPlazaRoots;
    constexpr F64 PLAZA_M = 20.0;               // wider than this both ways, an undecided slab is a surface, not road

    bool drivableStrict(const Shape& sh);

    // Is this shape the driving surface? Its textures match the surface's, or (on the chained road ahead)
    // it belongs to a linkset with no matching part at all.
    // Set while the walk looks past where the driving surface's textures ran out (see walk()).
    bool sAnySurface = false;

    bool drivable(const Shape& sh)
    {
        if (sh.piece && sPlazaRoots.count(sh.piece->root))
        {
            return false;       // a car park beside the road
        }
        if (sAnySurface)
        {
            return true;
        }
        if (!sAllowedRoots.empty() && sh.piece && sOpenRoots.count(sh.piece->root))
        {
            return true;
        }
        return drivableStrict(sh);
    }

    bool drivableStrict(const Shape& sh)
    {
        if (sDriveTex.empty() || !sh.piece || !sh.piece->textures_known)
        {
            return true;
        }
        for (const LLUUID& t : sh.piece->textures)
        {
            if (sDriveTex.count(t))
            {
                return true;
            }
        }
        return false;
    }

    bool inside(const Shape& sh, F64 x, F64 y)
    {
        F64 area = 0;
        for (S32 k = 0; k < sh.n; ++k)
        {
            area += sh.x[k] * sh.y[(k + 1) % sh.n] - sh.x[(k + 1) % sh.n] * sh.y[k];
        }
        for (S32 k = 0; k < sh.n; ++k)
        {
            const F64 ex = sh.x[(k + 1) % sh.n] - sh.x[k], ey = sh.y[(k + 1) % sh.n] - sh.y[k];
            if ((area > 0 ? 1 : -1) * (ex * (y - sh.y[k]) - ey * (x - sh.x[k])) < -0.05)
            {
                return false;
            }
        }
        return true;
    }
    std::vector<AirTile> sAirTiles;
    U64 sAirRevision = 0;
    Timer sSinceAir;

    // The air's walkable floor under a point: the highest no more than `up` above z and no more than `down`
    // below it, over every mesh's air there (a road's cluster and the terrain each have their own). Its height
    // and clearance (metres, 2 at most); false where the air has none.
    bool airFloor(F64 x, F64 y, F64 z, F64 up, F64 down, F64& floor, F64& clear)
    {
        bool found = false;
        static skworld::AirColumn col;
        for (const AirTile& a : sAirTiles)
        {
            if (x < a.x0 || x >= a.x1 || y < a.y0 || y >= a.y1)
            {
                continue;
            }
            const skworld::TileAir& air = *a.air;
            const S32 ix = S32((x - a.ox) / air.mCell), iy = S32((y - a.oy) / air.mCell);
            const S32 leaf = skworld::findLeafByKey(air, ix, iy);
            if (leaf < 0)
            {
                continue;
            }
            skworld::airColumn(air, leaf, col);
            for (U32 k = 0; k < col.mCount; ++k)
            {
                const skworld::AirGap& g = air.mGaps[col.mGap[k]];
                if (!(g.mFlags & skworld::AirGap::TOP))
                {
                    continue;       // the solid under it is not a floor
                }
                const F64 h = a.oz + col.floorAt(air, k, ix, iy) * air.mUnit;
                if (h - z < up && z - h < down && (!found || h > floor))
                {
                    found = true;
                    floor = h;
                    clear = g.mClear * skworld::CLEAR_UNIT_M;
                }
            }
        }
        return found;
    }

    // The shape's surface height at a point: on the plane through its corners (a long triangle on a hill
    // is metres off its mean height at its ends, which put the lane line under the road or over it).
    F64 heightAt(const Shape& sh, F64 x, F64 y)
    {
        const F64 ax = sh.x[1] - sh.x[0], ay = sh.y[1] - sh.y[0], az = sh.zv[1] - sh.zv[0];
        const F64 bx = sh.x[2] - sh.x[0], by = sh.y[2] - sh.y[0], bz = sh.zv[2] - sh.zv[0];
        const F64 nz = ax * by - ay * bx;
        if (std::fabs(nz) < 1e-9)
        {
            return sh.z;
        }
        const F64 nx = ay * bz - az * by, ny = az * bx - ax * bz;
        return sh.zv[0] - (nx * (x - sh.x[0]) + ny * (y - sh.y[0])) / nz;
    }

    // The road's surface under a point: the highest road shape there no more than MAX_STEP_UP_M above z (the
    // line's height carried on from the point before), so the top of the road and a cover laid on it, never a
    // base under it (it sank into the road) nor a post or a bollard on its linkset; or z where none is.
    constexpr F64 MAX_STEP_UP_M = 0.6;
    constexpr F64 MIN_AIR_CLEAR_M = 0.5;        // a lane point nearer a wall than this has left the road's corridor
    F64 surfaceAt(F64 x, F64 y, F64 z)
    {
        F64 floor, clear;
        if (airFloor(x, y, z, MAX_STEP_UP_M, Z_REACH, floor, clear))
        {
            return floor;
        }
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it == sHash.end())
        {
            return z;
        }
        F64 best = -1e9;
        for (S32 i : it->second)
        {
            const Shape& sh = *sShapes[i];
            if (!inside(sh, x, y))
            {
                continue;
            }
            const F64 h = heightAt(sh, x, y);
            if (h - z < MAX_STEP_UP_M && z - h < Z_REACH && h > best)
            {
                best = h;
            }
        }
        return best > -1e8 ? best : z;
    }

    bool roadTopAt(F64 x, F64 y, F64 z, F64 up, F64 down, F64& top)
    {
        bool found = false;
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it != sHash.end())
        {
            for (S32 i : it->second)
            {
                const Shape& sh = *sShapes[i];
                if (!inside(sh, x, y))
                {
                    continue;
                }
                const F64 h = heightAt(sh, x, y);
                if (h - z <= up && z - h <= down && (!found || h > top))
                {
                    top = h;
                    found = true;
                }
            }
        }
        return found;
    }

    S8 linksetAccess(const LLUUID& root)
    {
        auto known = sAccess.find(root);
        if (known != sAccess.end())
        {
            return known->second;
        }
        S8 access = ROAD_UNKNOWN;
        const host::Props* p = host::properties(root);
        if (p && (host::lindenOwner(p->mOwnerId) || p->mGroupId == LDPW || p->mGroupId == LINDEN_MAINTENANCE))
        {
            access = ROAD_PUBLIC;       // a Linden's, a mole's or the road groups' wherever it is rooted
        }
        else if (auto at = sRootAt.find(root); at != sRootAt.end())
        {
            // PARCEL_PUBLIC (0): the Linden land the public roads lie on
            const S32 own = host::parcelOwnership(at->second.mdV[VX], at->second.mdV[VY]);
            access = own < 0 ? ROAD_UNKNOWN : own == 0 ? ROAD_PUBLIC : ROAD_PRIVATE;
        }
        if (access == ROAD_PUBLIC || (access == ROAD_PRIVATE && p))
        {
            sAccess[root] = access;     // settled (a private verdict once its owner is known not to be a Linden)
        }
        return access;
    }

    S8 accessAt(F64 x, F64 y, F64 z)
    {
        S8 access = ROAD_UNKNOWN;
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it == sHash.end())
        {
            return access;
        }
        for (S32 i : it->second)
        {
            const Shape& sh = *sShapes[i];
            if (!sh.piece || !inside(sh, x, y) || std::fabs(heightAt(sh, x, y) - z) > 2.0)
            {
                continue;
            }
            const S8 a = linksetAccess(sh.piece->root);
            if (a == ROAD_PUBLIC)
            {
                return ROAD_PUBLIC;
            }
            access = std::max(access, a);
        }
        return access;
    }

    LLUUID rootAt(F64 x, F64 y, F64 z)
    {
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it == sHash.end())
        {
            return LLUUID::null;
        }
        LLUUID best;
        F64 best_dz = 2.0;
        for (S32 i : it->second)
        {
            const Shape& sh = *sShapes[i];
            if (!sh.piece || !inside(sh, x, y))
            {
                continue;
            }
            if (const F64 dz = std::fabs(heightAt(sh, x, y) - z); dz < best_dz)
            {
                best_dz = dz;
                best = sh.piece->root;
            }
        }
        return best;
    }

    const char* accessName(S8 access)
    {
        return access == ROAD_PUBLIC ? "public" : access == ROAD_PRIVATE ? "private" : "unknown";
    }

    // Whose road the line and the junctions ahead are on: the line's points (accessAt) and each branch (its linkset);
    // a private branch leaving a public road is a private road's mouth.
    void tagAccess()
    {
        for (LanePoint& q : sPath)
        {
            q.access = accessAt(q.x, q.y, q.z);
        }
        for (Junction& j : sView.junctions)
        {
            const S8 from = linksetAccess(j.from);
            for (Branch& b : j.branches)
            {
                b.access = linksetAccess(b.root);
                b.mouth = from == ROAD_PUBLIC && b.access == ROAD_PRIVATE;
            }
        }
    }

    // The top of the road pieces at a point, up to `up` above z (for drawing over a cover laid on the road).
    F64 topAt(F64 x, F64 y, F64 z, F64 up)
    {
        F64 top = z;
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it != sHash.end())
        {
            for (S32 i : it->second)
            {
                const Shape& sh = *sShapes[i];
                if (inside(sh, x, y))
                {
                    const F64 h = heightAt(sh, x, y);
                    if (h > top && h - z < up)
                    {
                        top = h;
                    }
                }
            }
        }
        return top;
    }

    // Is the point on any road piece, whatever its texture?
    bool onRoad(F64 x, F64 y, F64 z)
    {
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it == sHash.end())
        {
            return false;
        }
        for (S32 i : it->second)
        {
            const Shape& sh = *sShapes[i];
            if (std::fabs(sh.z - z) < Z_REACH && inside(sh, x, y))
            {
                return true;
            }
        }
        return false;
    }

    // The driving surface's textures, from the pieces under the car (see sDriveTex).
    void noteDriveSurface(F64 x, F64 y, F64 z, bool starting)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - noteDriveSurface");
        if (!starting && sSinceDriveTex.getElapsedTimeF64() < 0.5)
        {
            return;
        }
        sSinceDriveTex.reset();
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it == sHash.end())
        {
            return;
        }
        bool any_shares = false;
        std::unordered_set<LLUUID> here;
        for (S32 i : it->second)
        {
            const Shape& sh = *sShapes[i];
            if (!sh.piece || !sh.piece->textures_known || std::fabs(sh.z - z) > 2.0 || !inside(sh, x, y))
            {
                continue;
            }
            here.insert(sh.piece->textures.begin(), sh.piece->textures.end());
            bool shares = sDriveTex.empty() || starting;
            for (const LLUUID& t : sh.piece->textures)
            {
                shares = shares || sDriveTex.count(t);
            }
            if (shares)
            {
                any_shares = true;
                const size_t had = sDriveTex.size();
                sDriveTex.insert(sh.piece->textures.begin(), sh.piece->textures.end());
                if (sDriveTex.size() != had)
                {
                    driveTexChanged();
                }
            }
        }
        // On road pieces none of which are the surface, check after check: the surface the car was on has
        // ended and the road goes on in another (asphalt into cobbles). That one is the surface from here.
        sOffSurfaceChecks = (any_shares || here.empty()) ? 0 : sOffSurfaceChecks + 1;
        if (sOffSurfaceChecks >= 3)
        {
            sOffSurfaceChecks = 0;
            sDriveTex = std::move(here);
            driveTexChanged();
            LL_INFOS("SKRoadDriver") << "the road goes on in a new surface: " << sDriveTex.size() << " textures" << LL_ENDL;
        }
    }

    // The textures of the road pieces under a point, those whose textures are known.
    std::unordered_set<LLUUID> texturesAt(F64 x, F64 y, F64 z)
    {
        std::unordered_set<LLUUID> out;
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it == sHash.end())
        {
            return out;
        }
        for (S32 i : it->second)
        {
            const Shape& sh = *sShapes[i];
            if (sh.piece && sh.piece->textures_known && std::fabs(sh.z - z) < 2.0 && inside(sh, x, y))
            {
                out.insert(sh.piece->textures.begin(), sh.piece->textures.end());
            }
        }
        return out;
    }

    bool sharesAny(const std::unordered_set<LLUUID>& a, const std::unordered_set<LLUUID>& b)
    {
        for (const LLUUID& t : a)
        {
            if (b.count(t))
            {
                return true;
            }
        }
        return false;
    }

    // The interval of s in [lo, hi] where (x + rx s, y + ry s) lies in the convex shape, or false.
    bool clip(const Shape& sh, F64 x, F64 y, F64 rx, F64 ry, F64& lo, F64& hi)
    {
        F64 area = 0;
        for (S32 k = 0; k < sh.n; ++k)
        {
            area += sh.x[k] * sh.y[(k + 1) % sh.n] - sh.x[(k + 1) % sh.n] * sh.y[k];
        }
        const F64 sign = area > 0 ? 1.0 : -1.0;
        for (S32 k = 0; k < sh.n; ++k)
        {
            const F64 x0 = sh.x[k], y0 = sh.y[k];
            const F64 ex = sh.x[(k + 1) % sh.n] - x0, ey = sh.y[(k + 1) % sh.n] - y0;
            const F64 a = sign * (ex * (y - y0) - ey * (x - x0));
            const F64 b = sign * (ex * ry - ey * rx);
            if (std::fabs(b) < 1e-12)
            {
                if (a < 0)
                {
                    return false;
                }
                continue;
            }
            const F64 t = -a / b;
            if (b > 0)
            {
                lo = std::max(lo, t);
            }
            else
            {
                hi = std::min(hi, t);
            }
            if (lo > hi)
            {
                return false;
            }
        }
        return true;
    }

    // A cross-section's spans: each shape's interval across the line (metres right of the point), and the road's
    // height at its middle.
    std::vector<std::pair<F64, F64>> sSpans;
    std::vector<F64> sSpanZ;

    void addSpan(const Shape& sh, F64 x, F64 y, F64 rx, F64 ry)
    {
        F64 lo = -REACH_M, hi = REACH_M;
        if (clip(sh, x, y, rx, ry, lo, hi) && hi - lo > 1e-4)
        {
            sSpans.push_back({ lo, hi });
            const F64 m = (lo + hi) / 2;
            sSpanZ.push_back(heightAt(sh, x + rx * m, y + ry * m));
        }
    }

    // The spans merged into runs (across seams), and the run holding the point or nearest it.
    bool mergeSpans(F64& left, F64& right, F64& zroad)
    {
        if (sSpans.empty())
        {
            return false;
        }
        static std::vector<size_t> order;
        static std::vector<std::pair<F64, F64>> runs;
        static std::vector<F64> run_z;
        order.resize(sSpans.size());
        for (size_t i = 0; i < order.size(); ++i)
        {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [](size_t a, size_t b) { return sSpans[a].first < sSpans[b].first; });
        runs.clear();
        run_z.clear();
        for (size_t i : order)
        {
            if (!runs.empty() && sSpans[i].first <= runs.back().second + (sDriveTex.empty() ? SEAM_M : BRIDGE_M))
            {
                runs.back().second = std::max(runs.back().second, sSpans[i].second);
                run_z.back() = std::max(run_z.back(), sSpanZ[i]);
            }
            else
            {
                runs.push_back(sSpans[i]);
                run_z.push_back(sSpanZ[i]);
            }
        }
        size_t best = 0;
        F64 best_d = 1e9;
        for (size_t i = 0; i < runs.size(); ++i)
        {
            const F64 d = (runs[i].first <= 0 && 0 <= runs[i].second) ? 0.0
                          : std::min(std::fabs(runs[i].first), std::fabs(runs[i].second));
            if (d < best_d)
            {
                best_d = d;
                best = i;
            }
        }
        if (best_d > 6.0)
        {
            return false;
        }
        left = runs[best].first;
        right = runs[best].second;
        zroad = run_z[best];
        return true;
    }

    std::vector<S32> sSectionCands;

    // Whether a shape crosses the line across the road through (x, y) along (rx, ry) within REACH_M: its span
    // there would be added.
    bool spanCrosses(const Shape& sh, F64 x, F64 y, F64 rx, F64 ry)
    {
        const size_t before = sSpans.size();
        addSpan(sh, x, y, rx, ry);
        const bool crosses = sSpans.size() > before;
        sSpans.resize(before);
        sSpanZ.resize(std::min(sSpanZ.size(), before));
        return crosses;
    }

    // The road across (x, y) perpendicular to (hx, hy), at about height z: its edges in metres right of
    // the point (the merged run of pieces holding the point, or nearest it), and the road's height there.
    constexpr F64 SECTION_SQUARE_RATIO = 1.3;   // SQUARE_RATIO (declared with the segments, below)
    constexpr F64 WAY_ONLY_COS = 0.57;          // a piece's long axis more than 55 deg off the way lies across it
    bool section(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64& left, F64& right, F64& zroad)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - section");
        const F64 rx = hy, ry = -hx;
        static std::unordered_set<S32> seen;
        sSpans.clear();
        sSpanZ.clear();
        seen.clear();
        for (S64 gx = S64(std::floor((x - REACH_M) / HASH_M)); gx <= S64(std::floor((x + REACH_M) / HASH_M)); ++gx)
        {
            for (S64 gy = S64(std::floor((y - REACH_M) / HASH_M)); gy <= S64(std::floor((y + REACH_M) / HASH_M)); ++gy)
            {
                auto it = sHash.find(hashKey(gx, gy));
                if (it == sHash.end())
                {
                    continue;
                }
                for (S32 i : it->second)
                {
                    if (!seen.insert(i).second)
                    {
                        continue;
                    }
                    const Shape& sh = *sShapes[i];
                    if (std::fabs(sh.z - z) > Z_REACH)
                    {
                        continue;
                    }
                    if (!sAllowedRoots.empty() && (!sh.piece || !sAllowedRoots.count(sh.piece->root)))
                    {
                        continue;       // not on the chained road ahead
                    }
                    if (!drivable(sh))
                    {
                        continue;       // a verge or kerb built into the road object
                    }
                    if (sWayOnly && sh.piece && sh.piece != sWayKeep[0] && sh.piece != sWayKeep[1]
                        && std::max(sh.piece->lu, sh.piece->lv) >= SECTION_SQUARE_RATIO * std::min(sh.piece->lu, sh.piece->lv))
                    {
                        const F64 lx = sh.piece->lu >= sh.piece->lv ? sh.piece->ux : -sh.piece->uy;
                        const F64 ly = sh.piece->lu >= sh.piece->lv ? sh.piece->uy : sh.piece->ux;
                        if (std::fabs(lx * sWayX + ly * sWayY) < WAY_ONLY_COS)
                        {
                            continue;   // a side road's strip, across the way
                        }
                    }
                    sSectionCands.push_back(i);
                }
            }
        }
        // where a definite road piece (a Linden's, a road group's) lies across here, only definite road pieces
        // make the road's width: a wall top or a pavement that is only road-shaped, tacked on beside a JP road,
        // moved its centre line to the wall
        bool any_definite = false;
        for (S32 i : sSectionCands)
        {
            const Shape& sh = *sShapes[i];
            any_definite |= sh.piece && roadVerdict(sh.piece->root) == 1 && spanCrosses(sh, x, y, rx, ry);
        }
        for (S32 i : sSectionCands)
        {
            const Shape& sh = *sShapes[i];
            if (!any_definite || (sh.piece && roadVerdict(sh.piece->root) == 1))
            {
                addSpan(sh, x, y, rx, ry);
            }
        }
        sSectionCands.clear();
        return mergeSpans(left, right, zroad);
    }

    // section() on the given segments and the halves alongside them alone, every face of their linksets in reach
    // whatever its surface (sectionOn's callers are on the road's own pieces): their own faces walked, not
    // every piece's around the point.
    bool sectionOn(F64 x, F64 y, F64 z, F64 hx, F64 hy, std::initializer_list<S32> segments, F64& left, F64& right, F64& zroad);

    // The right lane's line, metres right of the road's centre line on a two-lane road: the car's half width and
    // LANE_MARGIN_M, as a driver keeps to the right of the centre line, whatever the road's width (Nexii's
    // ruling: the width only says whether there is room for two lanes). A quarter of the width measured put the
    // line wherever the measuring put the edges: a wall top or a pavement taken for road moved it over. `keep`
    // (SKRoadAutopilotLaneOffset) moves it further right, or toward the centre line.
    constexpr F64 LANE_MARGIN_M = 0.5;
    F64 sCarHalfW = 1.47;
    F64 laneShift(F64 keep) { return sCarHalfW + LANE_MARGIN_M + keep; }
    // On a road of `width`, no further from the centre than leaves the car LANE_MARGIN_M inside the edge: with two
    // lanes from TWO_LANES_M (6 m), a road under 7.9 m put the 2.9 m car's side over that margin or onto the kerb,
    // and a width hovering at TWO_LANES_M moved the line 2 m sideways between frames.
    F64 laneShiftIn(F64 width, F64 keep)
    {
        const F64 room = std::max(0.0, width / 2 - sCarHalfW - LANE_MARGIN_M);
        return std::clamp(laneShift(keep), -room, room);
    }

    // The right lane at a point: centre, direction, widths. Two lanes from TWO_LANES_M, the right lane's
    // middle a quarter of the width right of centre; `keep` moves the point right of the lane's middle on
    // two lanes (Nexii's line: -0.47).
    bool laneAt(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 keep, LanePoint& out)
    {
        F64 left, right, zr;
        if (!section(x, y, z, hx, hy, left, right, zr))
        {
            return false;
        }
        const F64 rx = hy, ry = -hx;
        const F64 width = right - left, centre = (left + right) / 2;
        const F64 cx = x + rx * centre, cy = y + ry * centre;
        F64 ax = hx, ay = hy;
        F64 l2, r2, z2, l3, r3, z3;
        if (section(cx - hx * DIR_BASE_M, cy - hy * DIR_BASE_M, zr, hx, hy, l2, r2, z2)
            && section(cx + hx * DIR_BASE_M, cy + hy * DIR_BASE_M, zr, hx, hy, l3, r3, z3))
        {
            const F64 c2 = (l2 + r2) / 2, c3 = (l3 + r3) / 2;
            const F64 dx = (cx + hx * DIR_BASE_M + rx * c3) - (cx - hx * DIR_BASE_M + rx * c2);
            const F64 dy = (cy + hy * DIR_BASE_M + ry * c3) - (cy - hy * DIR_BASE_M + ry * c2);
            const F64 L = std::hypot(dx, dy);
            if (L > 1e-6)
            {
                ax = dx / L;
                ay = dy / L;
            }
        }
        if (ax * hx + ay * hy < 0)
        {
            ax = -ax;
            ay = -ay;
        }
        const S32 lanes = width >= TWO_LANES_M ? 2 : 1;
        const F64 shift = lanes == 2 ? laneShiftIn(width, keep) : 0.0;
        const F64 lrx = ay, lry = -ax;          // right of travel along the road
        out.x = cx + lrx * shift;
        out.y = cy + lry * shift;
        out.ax = ax;
        out.ay = ay;
        out.lane_w = width / lanes;
        out.road_w = width;
        out.lanes = lanes;
        out.z = zr;
        out.left_x = x + rx * left;
        out.left_y = y + ry * left;
        out.right_x = x + rx * right;
        out.right_y = y + ry * right;
        applyKit(out, keep);
        return true;
    }

    // ---------------------------------------------------------------- kits (skroadkits.h)

    struct KitPlace
    {
        bool known = false;
        U64 hash = 0, key = 0;                  // its signature's, and its kit's (kitKey: lying this way up)
        const Kit* kit = nullptr;
        bool measuring = false;                 // its kit is being measured
        LLQuaternion rot;                       // the root's rotation
        F64 px = 0, py = 0, pz = 0;             // the root's global position
        F64 acx = 1, acy = 0;                   // the kit's across axis in plan, unit
        F64 tried = -100.0;                     // when it was last asked, while not known
    };
    std::unordered_map<LLUUID, KitPlace> sKitPlaces;
    constexpr F64 KIT_RETRY_S = 5.0;

    void forgetKitPlaces()
    {
        sKitPlaces.clear();
    }

    // Takes its kit once measured: a kit lying as a road (its across axis level), else none.
    void placeKit(KitPlace& kp)
    {
        kp.kit = kitFor(kp.key);
        kp.measuring = !kp.kit && kitAsked(kp.key) && !kitDone(kp.key);
        if (kp.kit)
        {
            LLVector3 a;
            a.mV[kp.kit->across] = 1.f;
            a = a * kp.rot;
            const F64 L = std::hypot(a.mV[VX], a.mV[VY]);
            if (L < 0.7)
            {
                kp.kit = nullptr;               // the kit's across axis is not level: not lying as a road
            }
            else
            {
                kp.acx = a.mV[VX] / L;
                kp.acy = a.mV[VY] / L;
            }
        }
    }

    const KitPlace& kitPlace(const LLUUID& root)
    {
        KitPlace& kp = sKitPlaces[root];
        if (kp.measuring)
        {
            placeKit(kp);
        }
        if (kp.known || clockSeconds() - kp.tried < KIT_RETRY_S)
        {
            return kp;
        }
        kp.tried = clockSeconds();
        LLVector3d pos;
        if (!host::kitOf(root, kp.hash, pos, kp.rot))
        {
            return kp;
        }
        kp.key = kitKey(kp.hash, kp.rot);
        if (!kitAsked(kp.key))
        {
            if (!host::measureKit(root, kp.key))
            {
                return kp;                      // its shape is not all in yet: asked again later
            }
            kitAsk(kp.key);
        }
        kp.known = true;
        kp.px = pos.mdV[VX];
        kp.py = pos.mdV[VY];
        kp.pz = pos.mdV[VZ];
        placeKit(kp);
        return kp;
    }

    bool sameKit(const LLUUID& a, const LLUUID& b)
    {
        if (a == b)
        {
            return true;
        }
        const KitPlace& ka = kitPlace(a);
        const KitPlace& kb = kitPlace(b);
        return ka.known && kb.known && ka.hash == kb.hash;
    }

    // The root of the road piece under a point (as accessAt finds them), null where none is.
    LLUUID rootAt(F64 x, F64 y, F64 z);

    bool applyKit(LanePoint& lp, F64 keep)
    {
        const F64 cx = (lp.left_x + lp.right_x) / 2, cy = (lp.left_y + lp.right_y) / 2;
        const LLUUID root = rootAt(cx, cy, lp.z);
        if (root.isNull())
        {
            return false;
        }
        const KitPlace& kp = kitPlace(root);
        if (!kp.kit || std::fabs(kp.acx * lp.ax + kp.acy * lp.ay) > 0.5)
        {
            return false;                       // not a measured kit, or crossed rather than driven along
        }
        const Kit& k = *kp.kit;
        const F64 s = (cx - kp.px) * kp.acx + (cy - kp.py) * kp.acy;
        if (s < k.kerb_min - 1.0 || s > k.kerb_max + 1.0)
        {
            return false;
        }
        const F64 rx = lp.ay, ry = -lp.ax;      // right of travel
        const F64 side = rx * kp.acx + ry * kp.acy > 0 ? 1.0 : -1.0;
        const F64 mid = (k.deck_min + k.deck_max) / 2;
        F64 lane = mid;
        for (F64 c : k.lanes)
        {
            if (k.lanes.size() > 1 && side * c > side * lane)
            {
                lane = c;                       // the right of travel's outermost lane centre
            }
        }
        const F64 bx = cx - kp.acx * s, by = cy - kp.acy * s;   // abreast of the root on the kit's across line
        const F64 lo = side > 0 ? k.deck_min : k.deck_max, hi = side > 0 ? k.deck_max : k.deck_min;
        lp.left_x = bx + kp.acx * lo;
        lp.left_y = by + kp.acy * lo;
        lp.right_x = bx + kp.acx * hi;
        lp.right_y = by + kp.acy * hi;
        lp.x = bx + kp.acx * lane + rx * keep;
        lp.y = by + kp.acy * lane + ry * keep;
        lp.road_w = k.deck_max - k.deck_min;
        lp.lanes = std::max<S32>(1, S32(k.lanes.size()));
        lp.lane_w = lp.road_w / lp.lanes;
        lp.lane_source = LANE_KIT;
        return true;
    }
    View sView;
    Timer sClock;
    std::vector<LanePoint> sPath;
    F64 sPathTime = -100.0;                     // when the path was last laid afresh
    std::deque<std::pair<F64, F64>> sDriven;     // the lane line under the car, latest last
    bool sHaveLastLine = false;
    F64 sLastLine[4] = { 0, 0, 1, 0 };          // its last point and direction before an open area
    F64 sLastSeenRoad = 0.0;

    void writeDriven();

    // ---------------------------------------------------------------- the lane path

    void chord(const std::vector<LanePoint>& pts, size_t i, S32 step, F64& ox, F64& oy)
    {
        size_t j = i;
        while (S64(j) + step >= 0 && S64(j) + step < S64(pts.size())
               && std::hypot(pts[j].x - pts[i].x, pts[j].y - pts[i].y) < CHORD_M)
        {
            j += step;
        }
        F64 ex = step > 0 ? pts[j].x - pts[i].x : pts[i].x - pts[j].x;
        F64 ey = step > 0 ? pts[j].y - pts[i].y : pts[i].y - pts[j].y;
        const F64 L = std::hypot(ex, ey);
        if (L > 1.0)
        {
            ox = ex / L;
            oy = ey / L;
        }
        else
        {
            ox = pts[i].ax;
            oy = pts[i].ay;
        }
    }

    bool drivenDir(F64& dx, F64& dy)
    {
        if (sDriven.size() < 2)
        {
            return false;
        }
        const auto [x1, y1] = sDriven.back();
        for (auto it = sDriven.rbegin(); it != sDriven.rend(); ++it)
        {
            const F64 L = std::hypot(x1 - it->first, y1 - it->second);
            if (L >= DRIVEN_M)
            {
                dx = (x1 - it->first) / L;
                dy = (y1 - it->second) / L;
                return true;
            }
        }
        return false;
    }

    bool chainAxisAt(F64 x, F64 y, F64 z, F64& ax, F64& ay);  // below, with the segments

    constexpr F64 NEW_SURFACE_M = 4.0;          // a new surface must run on this far to be a road's
    constexpr F64 SAME_ROAD_WIDTH = 1.3;        // as wide as before within this factor: the same road

    // The surface ran out, and a lane point was found on other road pieces: what is going on? If their own
    // surface runs on a few metres, it is a road and not a bridge's joint or a junction's patch. If the road's
    // shape goes on as well, as wide as before and in line with it, it is the same road re-surfaced
    // (asphalt into cobbles); otherwise another road starts here. Either way the walk takes its surface
    // from here on, and the view marks the change.
    bool newSurfaceAhead(const LanePoint& lp, const LanePoint* before, F64 dx, F64 dy)
    {
        if (sDriveTex.empty())
        {
            return false;
        }
        const std::unordered_set<LLUUID> here = texturesAt(lp.x, lp.y, lp.z);
        if (here.empty() || sharesAny(here, sDriveTex))
        {
            return false;
        }
        const std::unordered_set<LLUUID> on = texturesAt(lp.x + dx * NEW_SURFACE_M, lp.y + dy * NEW_SURFACE_M, lp.z);
        if (!sharesAny(here, on) || sharesAny(on, sDriveTex))
        {
            return false;       // not the same surface a few metres on: a joint or a patch, carried over
        }
        bool same_road = false;
        if (before && before->road_w < OPEN_W)
        {
            const F64 ratio = lp.road_w / std::max(0.1, before->road_w);
            const F64 aside = std::fabs((lp.x - before->x) * dy - (lp.y - before->y) * dx);
            same_road = ratio > 1 / SAME_ROAD_WIDTH && ratio < SAME_ROAD_WIDTH && aside < 1.0;
        }
        sDriveTex = here;
        driveTexChanged();
        if (sAllowedRoots.empty() && sView.surface_changes.empty())
        {
            sView.surface_changes.push_back({ lp.x, lp.y, lp.z, dx, dy, lp.road_w, same_road });
        }
        LL_DEBUGS("SKRoadDriver") << (same_road ? "the road is re-surfaced ahead" : "another road starts ahead") << LL_ENDL;
        return true;
    }

    // A lane point from every road piece, whatever its texture.
    bool anySurfaceLane(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 keep, LanePoint& out)
    {
        sAnySurface = true;
        const bool found = laneAt(x, y, z, hx, hy, keep, out);
        sAnySurface = false;
        return found;
    }

    // Walk the right lane's line ahead and filter it: open areas carry the line straight on, zigzags and
    // steps back are dropped, each point's direction comes from its neighbours.
    std::vector<LanePoint> walk(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 v, F64 keep)
    {
        const F64 length = lookAhead(v);
        std::vector<LanePoint> pts;
        F64 px = x, py = y, pz = z, dx = hx, dy = hy, travelled = 0;
        S32 off_road = 0;
        // the surface may change ahead (newSurfaceAhead); the car's own is put back after
        struct Restore
        {
            std::unordered_set<LLUUID> saved = sDriveTex;
            ~Restore()
            {
                sDriveTex = std::move(saved);
                driveTexChanged();
            }
        } restore;
        while (travelled <= length)
        {
            LanePoint lp;
            F64 sax = dx, say = dy;
            const bool on_axis = chainAxisAt(px, py, pz, sax, say);
            if (on_axis)
            {
                dx = sax;
                dy = say;
            }
            if (!laneAt(px, py, pz, dx, dy, keep, lp))
            {
                // The surface ran out ahead. Rather than stop, look at what is there: road pieces in other
                // textures (a bridge, a junction with another road) carry the line on, a step at a time,
                // until the surface's own textures take it back.
                sAnySurface = true;
                const bool found = laneAt(px, py, pz, dx, dy, keep, lp);
                sAnySurface = false;
                if (!found)
                {
                    break;
                }
                ++sView.other_surface;
                newSurfaceAhead(lp, pts.empty() ? nullptr : &pts.back(), dx, dy);
            }
            if (!pts.empty() && pts.back().road_w < OPEN_W && lp.road_w < SEAM_NARROW * pts.back().road_w)
            {
                // Much narrower than the step before: a section lying along the seam between two linksets,
                // whose ends are not straight (a kerb's curve), catches only part of the road. Slide along the
                // road for the full width; failing that, the last step's road carried on.
                LanePoint best = lp;
                for (F64 off : { SEAM_SLIDE_M, -SEAM_SLIDE_M, 2 * SEAM_SLIDE_M })
                {
                    LanePoint alt;
                    if (laneAt(px + dx * off, py + dy * off, pz, dx, dy, keep, alt) && alt.road_w > best.road_w)
                    {
                        best = alt;
                    }
                }
                if (best.road_w >= SEAM_NARROW * pts.back().road_w)
                {
                    // the slid section's lane, brought back abreast of this step
                    const F64 back = (best.x - px) * dx + (best.y - py) * dy;
                    best.x -= back * dx;
                    best.y -= back * dy;
                    best.left_x -= back * dx;
                    best.left_y -= back * dy;
                    best.right_x -= back * dx;
                    best.right_y -= back * dy;
                    lp = best;
                }
                else if (LanePoint any; anySurfaceLane(px, py, pz, dx, dy, keep, any)
                                              && any.road_w >= SEAM_NARROW * pts.back().road_w
                                              && newSurfaceAhead(any, &pts.back(), dx, dy))
                {
                    // not a seam: the surface's pieces end here and the road goes on in another
                    lp = any;
                }
                else
                {
                    const LanePoint& q = pts.back();
                    lp = q;
                    lp.x = q.x + q.ax * PATH_STEP;
                    lp.y = q.y + q.ay * PATH_STEP;
                    lp.left_x = q.left_x + q.ax * PATH_STEP;
                    lp.left_y = q.left_y + q.ay * PATH_STEP;
                    lp.right_x = q.right_x + q.ax * PATH_STEP;
                    lp.right_y = q.right_y + q.ay * PATH_STEP;
                }
            }
            if (!pts.empty() && lp.road_w < OPEN_W)
            {
                // continuity: where the line's own direction carries it, at most MAX_SHIFT_M aside
                const F64 rx = dy, ry = -dx;
                const F64 side = (lp.x - px) * rx + (lp.y - py) * ry;
                const F64 keep_side = std::clamp(side, -MAX_SHIFT_M, MAX_SHIFT_M);
                lp.x += (keep_side - side) * rx;
                lp.y += (keep_side - side) * ry;
            }
            if (on_axis && chainAxisAt(lp.x, lp.y, lp.z, sax, say))
            {
                lp.ax = sax;
                lp.ay = say;
            }
            {
                // the line bends gradually: a section that runs on into a car park beside the road tilts its
                // axis, and a step along a tilted axis leaves the road (seen walking off into a car park)
                const F64 limit = (pts.empty() ? MAX_START_TURN_DEG : MAX_STEP_TURN_DEG) * PI / 180;
                const F64 c = lp.ax * dx + lp.ay * dy, sn = dx * lp.ay - dy * lp.ax;
                const F64 a = std::atan2(sn, c);
                if (std::fabs(a) > limit)
                {
                    const F64 k = a > 0 ? limit : -limit;
                    lp.ax = dx * std::cos(k) - dy * std::sin(k);
                    lp.ay = dx * std::sin(k) + dy * std::cos(k);
                }
            }
            F64 air_floor, air_clear;
            const bool by_wall = airFloor(lp.x, lp.y, lp.z, MAX_STEP_UP_M, Z_REACH, air_floor, air_clear)
                                 && air_clear < MIN_AIR_CLEAR_M;
            if (lp.road_w < OPEN_W && (by_wall || !onRoad(lp.x, lp.y, lp.z)))
            {
                // the lane line is on no road piece at all: it has left the road, and nothing ahead of here is
                // the road's (seen running on across grass into a house)
                if (++off_road >= 2)
                {
                    break;
                }
            }
            else
            {
                off_road = 0;
            }
            pts.push_back(lp);
            px = lp.x + lp.ax * PATH_STEP;
            py = lp.y + lp.ay * PATH_STEP;
            pz = lp.z;
            dx = lp.ax;
            dy = lp.ay;
            travelled += PATH_STEP;
        }
        if (pts.empty())
        {
            return pts;
        }
        F64 odx, ody;
        const bool have_driven = drivenDir(odx, ody);
        if (pts[0].road_w >= OPEN_W)
        {
            // starting in an open area: the last lane line seen, abreast of the car, else its heading
            if (sHaveLastLine)
            {
                F64 lax = sLastLine[2], lay = sLastLine[3];
                if (have_driven)
                {
                    lax = odx;
                    lay = ody;
                }
                const F64 s = (x - sLastLine[0]) * lax + (y - sLastLine[1]) * lay;
                pts[0].x = sLastLine[0] + lax * s;
                pts[0].y = sLastLine[1] + lay * s;
                pts[0].ax = lax;
                pts[0].ay = lay;
            }
            else
            {
                pts[0].x = x;
                pts[0].y = y;
                pts[0].ax = hx;
                pts[0].ay = hy;
            }
        }
        if (pts[0].road_w < OPEN_W && pts[0].road_w < 1.3 * pts[0].lane_w * pts[0].lanes)
        {
            sDriven.push_back({ pts[0].x, pts[0].y });
            while (sDriven.size() > 40)
            {
                sDriven.pop_front();
            }
        }
        // filter
        std::vector<LanePoint> kept{ pts[0] };
        F64 kdx = pts[0].ax, kdy = pts[0].ay;
        const F64 cos_max = std::cos(MAX_TURN_DEG * PI / 180);
        bool opened = false;
        F64 ox = kdx, oy = kdy;
        for (size_t i = 1; i < pts.size(); ++i)
        {
            const LanePoint& p = pts[i];
            if (p.road_w >= OPEN_W)
            {
                if (!opened)
                {
                    opened = true;
                    if (have_driven)
                    {
                        ox = odx;
                        oy = ody;
                    }
                    else
                    {
                        chord(kept, kept.size() - 1, -1, ox, oy);
                    }
                }
                LanePoint q = kept.back();
                q.x += ox * PATH_STEP;
                q.y += oy * PATH_STEP;
                q.ax = ox;
                q.ay = oy;
                q.lane_w = p.lane_w;
                q.road_w = p.road_w;
                q.lanes = p.lanes;
                kept.push_back(q);
                kdx = ox;
                kdy = oy;
                continue;
            }
            opened = false;
            const F64 ex = p.x - kept.back().x, ey = p.y - kept.back().y;
            const F64 L = std::hypot(ex, ey);
            if (L < 0.3 * PATH_STEP || (ex * kdx + ey * kdy) / L < cos_max)
            {
                continue;
            }
            kdx = ex / L;
            kdy = ey / L;
            kept.push_back(p);
        }
        for (size_t i = 0; i < kept.size(); ++i)
        {
            const LanePoint& a = kept[i > 0 ? i - 1 : 0];
            const LanePoint& b = kept[std::min(kept.size() - 1, i + 1)];
            const F64 ex = b.x - a.x, ey = b.y - a.y;
            const F64 L = std::hypot(ex, ey);
            if (L > 1e-6)
            {
                kept[i].ax = ex / L;
                kept[i].ay = ey / L;
            }
        }
        if (kept[0].road_w < OPEN_W)
        {
            F64 cx, cy;
            chord(kept, 0, 1, cx, cy);
            sHaveLastLine = true;
            sLastLine[0] = kept[0].x;
            sLastLine[1] = kept[0].y;
            sLastLine[2] = cx;
            sLastLine[3] = cy;
        }
        // past the road's end, the last direction carried on
        while (kept.size() < 3 || (kept.size() - 1) * PATH_STEP < length)
        {
            LanePoint q = kept.back();
            q.x += q.ax * PATH_STEP;
            q.y += q.ay * PATH_STEP;
            kept.push_back(q);
        }
        return kept;
    }
    ActiveSegments sSegments;
    std::unordered_map<U64, std::vector<S32>> sSegEnds;     // 16 m cells -> segments with an end there
    std::unordered_map<LLUUID, S32> sSegByRoot;
    constexpr F64 JOIN_M = 3.0;                 // a segment's end this close to another's joins them
    constexpr F64 MAX_JOIN_TURN_DEG = 60.0;     // sharper continuations are turns, not the road ahead
    constexpr F64 SQUARE_RATIO = 1.3;           // longer than wide by less than this: no direction
    constexpr F64 SQUARE_SCORE = 0.9;           // a square one's continuation scores as a ~26 degree turn
    constexpr F64 SAME_SURFACE_BONUS = 0.1;     // a continuation in the driving surface's textures
    constexpr F64 STRAIGHT_FILL = 0.85;         // of its rectangle a straight linkset's faces fill
    constexpr F64 MIN_MATCH_SHARE = 0.4;        // of a linkset's surface its matching parts must be to match

    // The road's direction at a point inside a straight linkset of the chained road, from the linkset's own
    // axis: exact, where a cross-section's (over cobbles' ragged edges) wanders a little each step.
    bool chainAxisAt(F64 x, F64 y, F64 z, F64& ax, F64& ay)
    {
        if (sAllowedRoots.empty())
        {
            return false;
        }
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it == sHash.end())
        {
            return false;
        }
        for (S32 i : it->second)
        {
            const Shape& sh = *sShapes[i];
            if (!sh.piece || !sAllowedRoots.count(sh.piece->root) || std::fabs(sh.z - z) > Z_REACH || !inside(sh, x, y))
            {
                continue;
            }
            auto f = sSegByRoot.find(sh.piece->root);
            if (f != sSegByRoot.end() && sSegments[f->second].straight
                && std::fabs(sSegments[f->second].ax * ax + sSegments[f->second].ay * ay) > std::cos(30.0 * PI / 180))
            {
                // only a linkset along the way: a crossing across the road has its long axis across it
                const Segment& sg = sSegments[f->second];
                const bool flip = sg.ax * ax + sg.ay * ay < 0;
                ax = flip ? -sg.ax : sg.ax;
                ay = flip ? -sg.ay : sg.ay;
                return true;
            }
        }
        return false;
    }
    std::vector<std::vector<Join>> sJoins;      // by segment
    constexpr F64 JOIN_EDGE_TOL_M = 0.25;       // an edge this close to another's line lies on it (pieces placed by hand),
    constexpr F64 JOIN_EDGE_DZ_M = 0.25;        // this close in height,
    constexpr F64 JOIN_MIN_M = 0.3;             // sharing this much of it

    // ---------------------------------------------------------------- segments and joins, settled once

    // A linkset's segment and its joins are facts about its pieces, not about the car: each is worked out when
    // the linkset first comes into reach, and kept while its pieces stay as they were, out of reach for a while
    // too. A linkset whose pieces change (moved, reshaped, retextured, another census record, a verdict on what
    // is road, or the driving surface's textures picking other parts of it) is worked out again, and so are
    // its joins; nothing else is.
    struct JoinEdge
    {
        F64 x0, y0, z0, x1, y1, z1;
    };

    U64 sJoinsKey = 0;      // the roads in reach the joins were last worked out for

    struct RootEntry
    {
        LLUUID root;
        U64 key = 0;                            // its pieces, as its segment was made from them
        U64 shape_key = 0;                      // its pieces' shapes alone: what its joins were made from
        std::vector<PieceShapes*> pieces;       // all its parts in reach, by id
        bool valid = false;                     // its faces make a segment
        Segment seg;
        std::vector<JoinEdge> outer;            // the segment's outer edges: its faces' edges none other of it shares
        std::vector<JoinEdge> runs;             // the same, a straight run along a side's outline as one edge
        F64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;     // where a join with it can be
        F64 used = 0;
        S32 active = -1;                        // its index in sSegments while in reach
    };
    std::unordered_map<LLUUID, RootEntry> sRoots;
    std::vector<RootEntry*> sActiveRoots;

    struct PairJoin
    {
        LLUUID a, b;                            // the lower first
        U64 key_a = 0, key_b = 0;
        bool joined = false;
        Join j;
        F64 used = 0;
        F64 worked = -1e9;                      // when it was last worked out
    };
    std::unordered_map<U64, PairJoin> sPairs;
    constexpr F64 PAIR_RECHECK_S = 5.0;
    constexpr F64 JOIN_BUDGET_S = 0.003;        // a frame's time for working out joins
    constexpr S32 JOIN_MIN_PER_FRAME = 64;      // and at least this many a frame, whatever the time
    S32 sJoinWorked = 0;
    std::vector<U64> sJoinPairs;                // the pairs listed for the roads in reach (see the joins pass)
    size_t sJoinCursor = 0;                     // how far through them the joins are worked
    Timer sJoinBudget;
    // The road settled is the map being made: kept for the session (nothing forgotten for being out of reach),
    // the oldest dropped only past these, a safety net; and written to sk-road-graph.jsonl as it settles.
    constexpr F64 CACHE_KEEP_S = 1e12;
    constexpr size_t CACHE_MAX_ROOTS = 65536;
    constexpr size_t CACHE_MAX_PAIRS = 262144;
    constexpr F64 RUN_TOL_M = 0.01;             // an outline's corners this near the line between two others lie on it

    F64 faceArea(const Shape& sh)
    {
        F64 a = 0;
        for (S32 k = 0; k < sh.n; ++k)
        {
            a += 0.5 * (sh.x[k] * sh.y[(k + 1) % sh.n] - sh.x[(k + 1) % sh.n] * sh.y[k]);
        }
        return std::fabs(a);
    }

    F64 pieceArea(const PieceShapes& p)
    {
        F64 a = 0;
        for (const Shape& sh : p.shapes)
        {
            a += faceArea(sh);
        }
        return a;
    }

    // Does the part share a texture with the driving surface (drivableStrict, for all its faces)?
    bool pieceMatches(PieceShapes& p)
    {
        if (p.match_gen != sDriveTexGen)
        {
            p.match_gen = sDriveTexGen;
            p.matches = sDriveTex.empty() || !p.textures_known
                        || std::any_of(p.textures.begin(), p.textures.end(), [](const LLUUID& t) { return sDriveTex.count(t) > 0; });
        }
        return p.matches;
    }

    // The linkset's root's frame (see computeSegment), as part of what its segment is made from.
    U64 frameKey(const LLUUID& root)
    {
        LLQuaternion rot;
        if (!host::rootRotation(root, rot))
        {
            return 0;
        }
        LLVector3 ux = LLVector3::x_axis * rot;
        ux.mV[VZ] = 0.f;
        if (ux.normVec() <= 0.1f)
        {
            return 1;
        }
        return mix(U64(std::llround(ux.mV[VX] * 1e4)) * 0x9E3779B97F4A7C15ULL ^ U64(std::llround(ux.mV[VY] * 1e4)));
    }

    // Do the corners lie on one straight edge: every one between the ends within RUN_TOL_M of the line, in plan
    // and in height?
    bool straightRun(const std::vector<std::array<F64, 3>>& p)
    {
        const std::array<F64, 3>& a = p.front();
        const std::array<F64, 3>& b = p.back();
        const F64 L = std::hypot(b[0] - a[0], b[1] - a[1]);
        if (L < 1e-6)
        {
            return false;
        }
        for (size_t i = 1; i + 1 < p.size(); ++i)
        {
            const F64 t = ((p[i][0] - a[0]) * (b[0] - a[0]) + (p[i][1] - a[1]) * (b[1] - a[1])) / (L * L);
            const F64 off = (-(p[i][0] - a[0]) * (b[1] - a[1]) + (p[i][1] - a[1]) * (b[0] - a[0])) / L;
            if (t <= 0 || t >= 1 || std::fabs(off) > RUN_TOL_M || std::fabs(a[2] + (b[2] - a[2]) * t - p[i][2]) > RUN_TOL_M)
            {
                return false;
            }
        }
        return true;
    }

    // The segment's outer edges: its parts' side outlines' edges that no other side of it shares (a part's seam
    // with its neighbour in the linkset, a crease between two of its sides), each as given and run together
    // where they go on in a straight line.
    void computeEdges(RootEntry& r, const std::vector<const PieceShapes*>& parts)
    {
        static std::vector<U64> keys;
        static std::vector<std::array<F64, 3>> run;
        keys.clear();
        run.clear();
        for (const PieceShapes* p : parts)
        {
            for (const SideEdge& e : p->outline)
            {
                keys.push_back(e.key);
            }
        }
        std::sort(keys.begin(), keys.end());
        auto flush = [&r]()
        {
            if (run.size() >= 2)
            {
                r.runs.push_back({ run.front()[0], run.front()[1], run.front()[2], run.back()[0], run.back()[1], run.back()[2] });
            }
            run.clear();
        };
        for (const PieceShapes* p : parts)
        {
            for (const SideEdge& e : p->outline)
            {
                if (e.loop_start)
                {
                    flush();
                }
                const auto [lo, hi] = std::equal_range(keys.begin(), keys.end(), e.key);
                if (hi - lo != 1)
                {
                    flush();
                    continue;
                }
                r.outer.push_back({ e.x0, e.y0, e.z0, e.x1, e.y1, e.z1 });
                if (!run.empty())
                {
                    run.push_back({ e.x1, e.y1, e.z1 });
                    if (straightRun(run))
                    {
                        continue;
                    }
                    run.pop_back();
                    flush();
                }
                run.push_back({ e.x0, e.y0, e.z0 });
                run.push_back({ e.x1, e.y1, e.z1 });
            }
            flush();
        }
    }

    // A linkset is a segment: its drivable parts' faces fitted with a centre line, ends and width.
    void computeSegment(RootEntry& r)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - computeSegment");
        r.valid = false;
        r.seg = Segment();
        r.outer.clear();
        r.runs.clear();
        Segment& sg = r.seg;
        sg.root = r.root;
        // its matching parts (a Route 10 segment without its grass verges), or all of it when none match
        // Its matching parts must be most of it: a cobbled road whose kerbs share a texture with the asphalt
        // before it is not the kerbs alone (seen drifting off the road where asphalt turned to cobbles).
        F64 all_area = 0, match_area = 0;
        bool any_match = false;
        for (PieceShapes* p : r.pieces)
        {
            const F64 a = pieceArea(*p);
            all_area += a;
            if (pieceMatches(*p))
            {
                any_match = true;
                match_area += a;
            }
        }
        sg.matches = any_match && match_area >= MIN_MATCH_SHARE * all_area;
        std::vector<const PieceShapes*> parts;
        std::vector<const Shape*> shapes;
        for (PieceShapes* p : r.pieces)
        {
            if (!sg.matches || p->matches)
            {
                parts.push_back(p);
                for (const Shape& sh : p->shapes)
                {
                    shapes.push_back(&sh);
                }
            }
        }
        {
            // its main surface: the textures of the parts that cover most of it (a part's are all its faces': its
            // kerb's and sides' too)
            std::vector<std::pair<const std::vector<LLUUID>*, F64>> by_set;
            for (const PieceShapes* p : parts)
            {
                if (p->textures_known)
                {
                    sg.textures.insert(p->textures.begin(), p->textures.end());
                    by_set.push_back({ &p->textures, pieceArea(*p) });
                }
            }
            std::sort(by_set.begin(), by_set.end(), [](const auto& a, const auto& b) { return *a.first < *b.first; });
            F64 most = 0;
            for (size_t i = 0; i < by_set.size();)
            {
                F64 a = 0;
                size_t j = i;
                for (; j < by_set.size() && *by_set[j].first == *by_set[i].first; ++j)
                {
                    a += by_set[j].second;
                }
                if (a > most)
                {
                    most = a;
                    sg.surface = std::unordered_set<LLUUID>(by_set[i].first->begin(), by_set[i].first->end());
                }
                i = j;
            }
        }
        F64 mx = 0, my = 0, mz = 0;
        for (const Shape* sh : shapes)
        {
            for (S32 k = 0; k < sh->n; ++k)
            {
                sg.pts.push_back({ sh->x[k], sh->y[k] });
                mx += sh->x[k];
                my += sh->y[k];
            }
            mz = std::max(mz == 0 ? -1e9 : mz, sh->z);
        }
        if (sg.pts.size() < 3)
        {
            return;
        }
        mx /= sg.pts.size();
        my /= sg.pts.size();
        F64 sxx = 0, syy = 0, sxy = 0;
        for (const auto& [x, y] : sg.pts)
        {
            sxx += (x - mx) * (x - mx);
            syy += (y - my) * (y - my);
            sxy += (x - mx) * (y - my);
        }
        const F64 ang = 0.5 * std::atan2(2 * sxy, sxx - syy);
        sg.ax = std::cos(ang);
        sg.ay = std::sin(ang);
        F64 alo = 1e9, ahi = -1e9, clo = 1e9, chi = -1e9;
        for (const auto& [x, y] : sg.pts)
        {
            const F64 al = (x - mx) * sg.ax + (y - my) * sg.ay, ac = -(x - mx) * sg.ay + (y - my) * sg.ax;
            alo = std::min(alo, al);
            ahi = std::max(ahi, al);
            clo = std::min(clo, ac);
            chi = std::max(chi, ac);
        }
        const F64 mid = (clo + chi) / 2;
        sg.cx = mx - mid * sg.ay;
        sg.cy = my + mid * sg.ax;
        sg.lo = alo;
        sg.hi = ahi;
        sg.width = chi - clo;
        sg.z = mz;
        sg.square = (ahi - alo) < SQUARE_RATIO * sg.width;
        {
            F64 a = 0;
            for (const Shape* sh : shapes)
            {
                a += faceArea(*sh);
            }
            sg.straight = !sg.square && a >= STRAIGHT_FILL * (ahi - alo) * sg.width;

            // The linkset's own frame: a road kit's piece is a box, its top faces filling it, its root turned
            // along the road. The box its top faces make in the root's horizontal axes gives the road's
            // direction, ends and width exactly, and joins are its end faces meeting the next one's. Where the
            // faces do not fill that box (a curved piece) the fitted line stands.
            if (LLQuaternion rot; host::rootRotation(r.root, rot))
            {
                LLVector3 ux = LLVector3::x_axis * rot;
                ux.mV[VZ] = 0.f;
                if (ux.normVec() > 0.1f)
                {
                    const F64 bx = ux.mV[VX], by = ux.mV[VY];
                    F64 ulo = 1e9, uhi = -1e9, vlo = 1e9, vhi = -1e9;
                    for (const auto& [x, y] : sg.pts)
                    {
                        const F64 u = x * bx + y * by, v = -x * by + y * bx;
                        ulo = std::min(ulo, u);
                        uhi = std::max(uhi, u);
                        vlo = std::min(vlo, v);
                        vhi = std::max(vhi, v);
                    }
                    const F64 lu = uhi - ulo, lv = vhi - vlo;
                    if (a >= STRAIGHT_FILL * lu * lv && lu > 0.1 && lv > 0.1)
                    {
                        const F64 mu = (ulo + uhi) / 2, mv = (vlo + vhi) / 2;
                        const bool along_u = lu >= lv;
                        sg.cx = mu * bx - mv * by;
                        sg.cy = mu * by + mv * bx;
                        sg.ax = along_u ? bx : -by;
                        sg.ay = along_u ? by : bx;
                        const F64 length = along_u ? lu : lv;
                        sg.width = along_u ? lv : lu;
                        sg.lo = -length / 2;
                        sg.hi = length / 2;
                        sg.square = length < SQUARE_RATIO * sg.width;
                        sg.straight = !sg.square;
                        sg.boxed = true;
                    }
                }
            }
        }
        {
            // height along the axis: a line fitted through its faces' centres
            F64 n = 0, sa = 0, sz = 0, saa = 0, saz = 0;
            for (const Shape* sh : shapes)
            {
                F64 fx = 0, fy = 0;
                for (S32 k = 0; k < sh->n; ++k)
                {
                    fx += sh->x[k] / sh->n;
                    fy += sh->y[k] / sh->n;
                }
                const F64 a = (fx - sg.cx) * sg.ax + (fy - sg.cy) * sg.ay;
                n += 1;
                sa += a;
                sz += sh->z;
                saa += a * a;
                saz += a * sh->z;
            }
            const F64 den = n * saa - sa * sa;
            sg.zslope = (!sg.square && den > 1e-6) ? (n * saz - sa * sz) / den : 0.0;
            // The fit through faces' centres bunched along the axis (an upright piece's sides, a short stack) is
            // ill-conditioned: a near-zero spread with a height difference gave slopes that, run out to the segment's
            // ends, put its road 100 m in the air. Its rise end to end may not exceed what its faces span in height,
            // nor a 50% grade.
            if (sg.zslope != 0.0 && !shapes.empty())
            {
                F64 zlo = 1e18, zhi = -1e18;
                for (const Shape* sh : shapes)
                {
                    for (S32 k = 0; k < sh->n; ++k)
                    {
                        zlo = std::min(zlo, sh->zv[k]);
                        zhi = std::max(zhi, sh->zv[k]);
                    }
                }
                const F64 length = std::max(sg.hi - sg.lo, 0.1);
                const F64 cap = std::min(0.5, (zhi - zlo + 0.1) / length);
                if (std::fabs(sg.zslope) > cap)
                {
                    LL_DEBUGS("SKRoad") << "segment " << sg.root << " slope " << sg.zslope << " capped to " << cap
                                        << " (length " << length << ", faces' heights " << zlo << ".." << zhi << ")" << LL_ENDL;
                    sg.zslope = std::copysign(cap, sg.zslope);
                }
            }
            sg.zc = (sz - sg.zslope * sa) / n;
        }
        sg.shapes = std::move(shapes);
        for (const PieceShapes* p : parts)
        {
            F64 sx = 0, sy = 0, n = 0;
            for (const Shape& sh : p->shapes)
            {
                for (S32 k = 0; k < sh.n; ++k)
                {
                    sx += sh.x[k];
                    sy += sh.y[k];
                    n += 1;
                }
            }
            sg.part_mids.push_back({ sx / n, sy / n });
        }
        r.valid = true;
        // its outline, each face once (a part duplicating another adds nothing)
        std::vector<const PieceShapes*> distinct;
        for (const PieceShapes* p : parts)
        {
            if (std::none_of(distinct.begin(), distinct.end(), [p](const PieceShapes* q) { return q->faces == p->faces; }))
            {
                distinct.push_back(p);
            }
        }
        computeEdges(r, distinct);
        // a join with it lies within its faces' reach: inside()'s tolerance, an edge's off its line
        r.x0 = r.y0 = 1e18;
        r.x1 = r.y1 = -1e18;
        for (const PieceShapes* p : r.pieces)
        {
            for (const Shape& sh : p->shapes)
            {
                r.x0 = std::min(r.x0, sh.tx0);
                r.y0 = std::min(r.y0, sh.ty0);
                r.x1 = std::max(r.x1, sh.tx1);
                r.y1 = std::max(r.y1, sh.ty1);
            }
        }
        r.x0 -= JOIN_EDGE_TOL_M + 0.01;
        r.y0 -= JOIN_EDGE_TOL_M + 0.01;
        r.x1 += JOIN_EDGE_TOL_M + 0.01;
        r.y1 += JOIN_EDGE_TOL_M + 0.01;
    }

    // Where two linksets' top faces meet: their outer edges lying on each other (in line, level, overlapping),
    // and an outer edge of one lying on the other's top face (a kit's pieces laid over each other's ends, an end
    // cap). Summed over all they share: its middle weighted by length, its extent along the first edge's line.
    bool joinPair(const RootEntry& ra, const RootEntry& rb, Join& j)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - joinPair");
        j = Join();
        for (const JoinEdge& e : ra.runs)
        {
            const F64 L = std::hypot(e.x1 - e.x0, e.y1 - e.y0);
            if (L < 1e-3)
            {
                continue;
            }
            const F64 ex0 = std::min(e.x0, e.x1) - JOIN_EDGE_TOL_M, ex1 = std::max(e.x0, e.x1) + JOIN_EDGE_TOL_M;
            const F64 ey0 = std::min(e.y0, e.y1) - JOIN_EDGE_TOL_M, ey1 = std::max(e.y0, e.y1) + JOIN_EDGE_TOL_M;
            const F64 ux = (e.x1 - e.x0) / L, uy = (e.y1 - e.y0) / L;
            for (const JoinEdge& f : rb.runs)
            {
                if (std::max(f.x0, f.x1) < ex0 || std::min(f.x0, f.x1) > ex1 || std::max(f.y0, f.y1) < ey0 || std::min(f.y0, f.y1) > ey1)
                {
                    continue;
                }
                // f's ends off e's line, and along it
                const F64 d0 = -(f.x0 - e.x0) * uy + (f.y0 - e.y0) * ux, d1 = -(f.x1 - e.x0) * uy + (f.y1 - e.y0) * ux;
                if (std::fabs(d0) > JOIN_EDGE_TOL_M || std::fabs(d1) > JOIN_EDGE_TOL_M)
                {
                    continue;
                }
                const F64 s0 = (f.x0 - e.x0) * ux + (f.y0 - e.y0) * uy, s1 = (f.x1 - e.x0) * ux + (f.y1 - e.y0) * uy;
                const F64 lo = std::max(0.0, std::min(s0, s1)), hi = std::min(L, std::max(s0, s1));
                if (hi - lo < JOIN_MIN_M)
                {
                    continue;
                }
                const F64 m = (lo + hi) / 2;
                const F64 ze = e.z0 + (e.z1 - e.z0) * m / L;
                const F64 tf = (std::fabs(s1 - s0) > 1e-6) ? (m - s0) / (s1 - s0) : 0.5;
                const F64 zf = f.z0 + (f.z1 - f.z0) * tf;
                if (std::fabs(ze - zf) > JOIN_EDGE_DZ_M)
                {
                    continue;
                }
                const F64 w = hi - lo, tot = j.length + w;
                if (j.length == 0)
                {
                    j.ax = e.x0;
                    j.ay = e.y0;
                    j.ex = ux;
                    j.ey = uy;
                }
                {
                    // the extent along the first edge's line, from its start, in the first edge's direction (the
                    // two pieces' outlines run opposite ways: in each edge's own, half the extent fell on the
                    // wrong side and it came out twice as wide)
                    const F64 p0 = (e.x0 + ux * lo - j.ax) * j.ex + (e.y0 + uy * lo - j.ay) * j.ey;
                    const F64 p1 = (e.x0 + ux * hi - j.ax) * j.ex + (e.y0 + uy * hi - j.ay) * j.ey;
                    j.lo = std::min(j.lo, std::min(p0, p1));
                    j.hi = std::max(j.hi, std::max(p0, p1));
                }
                j.x = (j.x * j.length + (e.x0 + ux * m) * w) / tot;
                j.y = (j.y * j.length + (e.y0 + uy * m) * w) / tot;
                j.z = (j.z * j.length + ze * w) / tot;
                j.length = tot;
            }
        }
        // Overlapping ends: an outer edge's middle on the other's top face, level with it. Each such edge is
        // shared ground.
        auto overlaps = [&j](const RootEntry& from, const RootEntry& onto)
        {
            for (const JoinEdge& e : from.outer)
            {
                const F64 L = std::hypot(e.x1 - e.x0, e.y1 - e.y0);
                if (L < JOIN_MIN_M)
                {
                    continue;
                }
                const F64 mx = (e.x0 + e.x1) / 2, my = (e.y0 + e.y1) / 2, mz = (e.z0 + e.z1) / 2;
                if (mx < onto.x0 || mx > onto.x1 || my < onto.y0 || my > onto.y1)
                {
                    continue;
                }
                bool on = false;
                for (const PieceShapes* p : onto.pieces)
                {
                    for (const Shape& sh : p->shapes)
                    {
                        if (mx >= sh.tx0 && mx <= sh.tx1 && my >= sh.ty0 && my <= sh.ty1 && inside(sh, mx, my)
                            && std::fabs(heightAt(sh, mx, my) - mz) <= JOIN_EDGE_DZ_M)
                        {
                            on = true;
                            break;
                        }
                    }
                    if (on)
                    {
                        break;
                    }
                }
                if (!on)
                {
                    continue;
                }
                const F64 ux = (e.x1 - e.x0) / L, uy = (e.y1 - e.y0) / L;
                if (j.length == 0)
                {
                    j.ax = e.x0;
                    j.ay = e.y0;
                    j.ex = ux;
                    j.ey = uy;
                }
                // along the first edge's line where it is roughly parallel; its middle weighted by length
                const F64 p0 = (e.x0 - j.ax) * j.ex + (e.y0 - j.ay) * j.ey, p1 = (e.x1 - j.ax) * j.ex + (e.y1 - j.ay) * j.ey;
                if (std::fabs(ux * j.ex + uy * j.ey) > 0.9)
                {
                    j.lo = std::min(j.lo, std::min(p0, p1));
                    j.hi = std::max(j.hi, std::max(p0, p1));
                }
                const F64 tot = j.length + L;
                j.x = (j.x * j.length + mx * L) / tot;
                j.y = (j.y * j.length + my * L) / tot;
                j.z = (j.z * j.length + mz * L) / tot;
                j.length = tot;
            }
        };
        overlaps(ra, rb);
        overlaps(rb, ra);
        if (j.length <= 0)
        {
            return false;
        }
        // what they share, end to end, its middle the extent's
        const F64 sx = j.ax, sy = j.ay;
        j.ax = sx + j.ex * j.lo;
        j.ay = sy + j.ey * j.lo;
        j.bx = sx + j.ex * j.hi;
        j.by = sy + j.ey * j.hi;
        return true;
    }

    // The join of two linksets in reach (a's root the lower), from the cache while both are as they were.
    const Join* pairJoin(const RootEntry& a, const RootEntry& b, F64 now, bool& deferred)
    {
        deferred = false;
        const U64 id = mix(std::hash<LLUUID>{}(a.root)) ^ (U64(std::hash<LLUUID>{}(b.root)) * 0x9E3779B97F4A7C15ULL);
        auto found = sPairs.find(id);
        if (found == sPairs.end() && sJoinBudget.getElapsedTimeF64() > JOIN_BUDGET_S && sJoinWorked >= JOIN_MIN_PER_FRAME)
        {
            deferred = true;    // put off without an entry (entries for pairs put off filled the cache past its cap)
            return nullptr;
        }
        PairJoin& p = found != sPairs.end() ? found->second : sPairs[id];
        // a join once found is settled (Nexii's ruling): never worked out again however the two roads' pieces in
        // reach come and go; two that have not joined, again only when their pieces changed and no sooner than
        // PAIR_RECHECK_S after the last time (a long linkset's parts stream in and out of reach as the car drives,
        // and every pair with it was worked out again each rebuild: 95 000 a rebuild, Tracy 2026-11-05-1906)
        const bool same_pair = p.a == a.root && p.b == b.root;
        const bool changed = !same_pair || p.key_a != a.shape_key || p.key_b != b.shape_key;
        const bool due = !same_pair || (changed && !p.joined && now - p.worked >= PAIR_RECHECK_S);
        if (due && sJoinBudget.getElapsedTimeF64() > JOIN_BUDGET_S && sJoinWorked >= JOIN_MIN_PER_FRAME)
        {
            // out of this frame's time: left for the next (logging in, every road arrives at once with none of its
            // joins known, and working them all out in one frame stalled it)
            deferred = true;
            return nullptr;
        }
        if (due)
        {
            ++sJoinWorked;
            p.worked = now;
            p.a = a.root;
            p.b = b.root;
            p.key_a = a.shape_key;
            p.key_b = b.shape_key;
            p.joined = joinPair(a, b, p.j);
        }
        p.used = now;
        return p.joined ? &p.j : nullptr;
    }

    // What is not used for a while goes, the oldest first past the caps.
    void evictCaches(F64 now)
    {
        static Timer since;
        if (since.getElapsedTimeF64() < 30.0 && sRoots.size() <= CACHE_MAX_ROOTS && sPairs.size() <= CACHE_MAX_PAIRS)
        {
            return;
        }
        since.reset();
        std::vector<std::pair<F64, LLUUID>> old;
        for (auto it = sRoots.begin(); it != sRoots.end();)
        {
            if (it->second.active < 0 && now - it->second.used > CACHE_KEEP_S)
            {
                it = sRoots.erase(it);
            }
            else
            {
                if (it->second.active < 0)
                {
                    old.push_back({ it->second.used, it->first });
                }
                ++it;
            }
        }
        if (sRoots.size() > CACHE_MAX_ROOTS)
        {
            std::sort(old.begin(), old.end());
            for (size_t i = 0; i < old.size() && sRoots.size() > CACHE_MAX_ROOTS * 3 / 4; ++i)
            {
                sRoots.erase(old[i].second);
            }
        }
        std::vector<std::pair<F64, U64>> old_pairs;
        for (auto it = sPairs.begin(); it != sPairs.end();)
        {
            if (now - it->second.used > CACHE_KEEP_S)
            {
                it = sPairs.erase(it);
            }
            else
            {
                old_pairs.push_back({ it->second.used, it->first });
                ++it;
            }
        }
        if (sPairs.size() > CACHE_MAX_PAIRS)
        {
            std::sort(old_pairs.begin(), old_pairs.end());
            for (size_t i = 0; i < old_pairs.size() && sPairs.size() > CACHE_MAX_PAIRS * 3 / 4; ++i)
            {
                sPairs.erase(old_pairs[i].second);
            }
        }
        // a part goes only once every linkset made from it has (a linkset in use was in reach no longer ago
        // than its parts were)
        for (auto it = sShapeCache.begin(); it != sShapeCache.end();)
        {
            it = now - it->second.used > CACHE_KEEP_S ? sShapeCache.erase(it) : std::next(it);
        }
        while (!sRetiredShapes.empty() && now - sRetiredShapes.front().first > CACHE_KEEP_S)
        {
            sRetiredShapes.pop_front();
        }
    }

    // The segments in reach and their joins, from the linksets gathered: each linkset's segment worked out only
    // where its pieces changed, each pair's join only where either did.
    void settleParts(F64 now);
    void rootPairsFromParts(std::vector<U64>& out);

    void settleSegments(bool force)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - settleSegments");
        const F64 now = sCacheClock.getElapsedTimeF64();
        // what each linkset's segment is made from, and what says whether it is road: where none of it moved,
        // the segments in reach stand as they are
        static U64 last_print = 0;
        U64 print = 0;
        for (Gathered& g : sGathered)
        {
            if (!roadOf(g) || g.pieces.empty())
            {
                continue;
            }
            g.key = frameKey(g.root);
            for (PieceShapes* p : g.pieces)
            {
                p->used = now;      // in use: kept (evictCaches)
                g.key += mix(pieceKey(*p) ^ (p->textures_known ? 0x9E3779B97F4A7C15ULL : 0) ^ (pieceMatches(*p) ? 0xC2B2AE3D27D4EB4FULL : 0));
            }
            const bool props = host::hasProperties(g.root);
            print += mix(g.key ^ (U64(std::hash<LLUUID>{}(g.root)) * 0x9E3779B97F4A7C15ULL) ^ U64(g.named + 2) ^ (props ? 8 : 0));
        }
        if (!force && print == last_print && !sJoinsIncomplete)
        {
            for (RootEntry* r : sActiveRoots)
            {
                r->used = now;
            }
            return;
        }
        last_print = print;
        for (RootEntry* r : sActiveRoots)
        {
            r->active = -1;
        }
        sActiveRoots.clear();
        sSegments.list.clear();
        sSegEnds.clear();
        sSegByRoot.clear();
        sOpenRoots.clear();
        sPlazaRoots.clear();
        sSiblingsKnown.clear();
        for (const Gathered& g : sGathered)
        {
            if (!roadOf(g) || g.pieces.empty())
            {
                continue;
            }
            const U64 key = g.key;
            auto [it, fresh] = sRoots.try_emplace(g.root);
            RootEntry& r = it->second;
            r.used = now;
            if (fresh || r.key != key || r.pieces != g.pieces)
            {
                r.root = g.root;
                r.key = key;
                r.pieces = g.pieces;
                // its shape alone, for its joins: the pieces, not whether their textures match the surface driven
                // on (that set changes as the car drives, and with it every key: every settled join was worked
                // out again, 144 000 a rebuild)
                r.shape_key = frameKey(g.root);
                for (const PieceShapes* pc : g.pieces)
                {
                    r.shape_key += mix(pieceKey(*pc));
                }
                computeSegment(r);
            }
            if (!r.seg.matches)
            {
                sOpenRoots.insert(g.root);
            }
            if (!r.valid)
            {
                continue;
            }
            Segment& sg = r.seg;
            sg.definite = definiteOf(g);
            sg.pending = !sg.definite && !host::hasProperties(g.root);
            if (!sg.definite && sg.square && sg.width > PLAZA_M)
            {
                // only road-shaped, square and wide: a car park, a plaza, a forecourt; not a road
                sPlazaRoots.insert(g.root);
                continue;
            }
            const S32 idx = S32(sSegments.size());
            r.active = idx;
            sActiveRoots.push_back(&r);
            sSegments.list.push_back(&sg);
            sSegByRoot[g.root] = idx;
            for (F64 e : { sg.lo, sg.hi })
            {
                const F64 ex = sg.cx + e * sg.ax, ey = sg.cy + e * sg.ay;
                sSegEnds[hashKey(S64(std::floor(ex / HASH_M)), S64(std::floor(ey / HASH_M)))].push_back(idx);
            }
            if (sg.square)
            {
                // a square one is reached from any side: index it by its centre too
                sSegEnds[hashKey(S64(std::floor(sg.cx / HASH_M)), S64(std::floor(sg.cy / HASH_M)))].push_back(idx);
            }
        }
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - joins");
            const S32 n = S32(sActiveRoots.size());
            // the roads in reach, in order, as their segments were made: the same as last time, the joins
            // between them are too (every pair is settled and cached, but walking every pair of them again on
            // every rebuild grew with the square of the roads in reach and took most of the frame)
            // The roads that can join are the roads whose parts meet: settleParts has found which parts meet
            // which (edge to edge, or laid over each other's ends), cached a pair at a time, so the candidate pairs
            // are read off its meetings, a few hundred, with no listing of every pair of roads in reach (that
            // listing was the cost: 96 % of the joins pass, Tracy 2026-11-05-1923; Fable adversarial review).
            // Listed again only when the roads in reach or their pieces change: keyed on the roots and their
            // piece ids, not frameKey (an object streaming in or out flipped it every frame and restarted the
            // list each time).
            settleParts(now);
            U64 set_key = U64(n);
            for (const RootEntry* r : sActiveRoots)
            {
                U64 pk = U64(std::hash<LLUUID>{}(r->root));
                for (const PieceShapes* pc : r->pieces)
                {
                    pk += mix(pieceKey(*pc));
                }
                set_key = mix(set_key ^ pk);
            }
            if (set_key != sJoinsKey || S32(sJoins.size()) != n)
            {
                sJoinsKey = set_key;
                sJoins.assign(n, {});
                sJoinPairs.clear();
                sJoinCursor = 0;
                rootPairsFromParts(sJoinPairs);
            }
            // the pairs listed, worked through from where the last frame stopped: the budget is on working joins
            // out alone, and at least JOIN_MIN_PER_FRAME are worked a frame whatever the time (with the budget
            // reset before the listing, which alone took 37 ms-1.5 s, every pair was put off, every frame,
            // and none was ever worked out: 1 joinPair in 56 s, the joins pass every frame, Tracy 2026-11-05-1917)
            sJoinBudget.reset();
            sJoinWorked = 0;
            sJoinsIncomplete = false;
            for (; sJoinCursor < sJoinPairs.size(); ++sJoinCursor)
            {
                const U64 ab = sJoinPairs[sJoinCursor];
                const S32 a = S32(ab >> 32), b = S32(ab & 0xffffffffu);
                const RootEntry& ra = *sActiveRoots[a];
                const RootEntry& rb = *sActiveRoots[b];
                const bool a_first = ra.root < rb.root;
                bool deferred = false;
                const Join* j = pairJoin(a_first ? ra : rb, a_first ? rb : ra, now, deferred);
                if (deferred)
                {
                    sJoinsIncomplete = true;    // this pair and the rest: the next frame
                    break;
                }
                if (j)
                {
                    Join out = *j;
                    out.other = b;
                    sJoins[a].push_back(out);
                    out.other = a;
                    sJoins[b].push_back(out);
                }
            }
        }
        evictCaches(now);
    }
    // The segment's surface height at a point (on its axis line).
    F64 zAt(const Segment& sg, F64 x, F64 y)
    {
        return sg.zc + sg.zslope * ((x - sg.cx) * sg.ax + (y - sg.cy) * sg.ay);
    }

    constexpr F64 SIBLING_DEG = 10.0;           // as parallel as this,
    constexpr F64 SIBLING_GAP_M = 1.5;          // edge to edge within this, overlapping along: one road

    // The segments alongside one: a road built of two linksets side by side, a half each (or unlinked by
    // mistake), is one road, and its cross-sections must see both halves.
    std::vector<S32> siblingsUncached(S32 idx);

    // The halves alongside a segment, worked out once a build (they were found by a scan over every segment,
    // many times a path: the square of the pieces in reach).
    const std::vector<S32>& siblings(S32 idx)
    {
        if (sSiblingsKnown.size() != sSegments.size())
        {
            sSiblings.assign(sSegments.size(), {});
            sSiblingsKnown.assign(sSegments.size(), false);
        }
        if (!sSiblingsKnown[idx])
        {
            sSiblings[idx] = siblingsUncached(idx);
            sSiblingsKnown[idx] = true;
        }
        return sSiblings[idx];
    }

    std::vector<S32> siblingsUncached(S32 idx)
    {
        std::vector<S32> out;
        const Segment& a = sSegments[idx];
        if (a.square)
        {
            return out;
        }
        for (S32 j = 0; j < S32(sSegments.size()); ++j)
        {
            const Segment& b = sSegments[j];
            if (j == idx || b.square || std::fabs(a.ax * b.ax + a.ay * b.ay) < std::cos(SIBLING_DEG * PI / 180))
            {
                continue;
            }
            // b's centre across and along a's axis
            const F64 across = std::fabs(-(b.cx - a.cx) * a.ay + (b.cy - a.cy) * a.ax);
            const F64 along = (b.cx - a.cx) * a.ax + (b.cy - a.cy) * a.ay;
            const F64 b_half = (b.hi - b.lo) / 2;
            const bool overlaps = along + b_half > a.lo && along - b_half < a.hi;
            if (overlaps && across - (a.width + b.width) / 2 < SIBLING_GAP_M && across > 0.5
                && std::fabs(zAt(a, b.cx, b.cy) - zAt(b, b.cx, b.cy)) < 0.5)
            {
                out.push_back(j);
            }
        }
        return out;
    }

    // The whole road across at a point on a segment, it and the halves alongside it: its extent across
    // (rx, ry is the right-hand normal of dx, dy) from the point, and its width.
    void roadAcross(S32 idx, F64 x, F64 y, F64 dx, F64 dy, F64& lo, F64& hi)
    {
        const F64 rx = dy, ry = -dx;
        lo = 1e9;
        hi = -1e9;
        std::vector<S32> all = siblings(idx);
        all.push_back(idx);
        for (S32 k : all)
        {
            const Segment& g = sSegments[k];
            const F64 c = (g.cx - x) * rx + (g.cy - y) * ry;
            lo = std::min(lo, c - g.width / 2);
            hi = std::max(hi, c + g.width / 2);
        }
    }

    // The segment the car is on: the one whose drivable faces hold the point.
    S32 segmentUnder(F64 x, F64 y, F64 z)
    {
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it == sHash.end())
        {
            return -1;
        }
        S32 any = -1;
        for (S32 i : it->second)
        {
            const Shape& sh = *sShapes[i];
            if (sh.piece && drivable(sh) && std::fabs(sh.z - z) < 2.5 && inside(sh, x, y))
            {
                auto f = sSegByRoot.find(sh.piece->root);
                if (f != sSegByRoot.end())
                {
                    if (sSegments[f->second].definite)
                    {
                        return f->second;
                    }
                    any = any < 0 ? f->second : any;
                }
            }
        }
        return any;
    }

    // How far a square segment reaches from (x, y) along (dx, dy).
    F64 reach(const Segment& sg, F64 x, F64 y, F64 dx, F64 dy)
    {
        F64 extent = 0;
        for (const auto& [px, py] : sg.pts)
        {
            extent = std::max(extent, (px - x) * dx + (py - y) * dy);
        }
        return extent;
    }

    bool sectionOn(F64 x, F64 y, F64 z, F64 hx, F64 hy, std::initializer_list<S32> segments, F64& left, F64& right, F64& zroad)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - sectionOn");
        const F64 rx = hy, ry = -hx;
        // the line's bounds: a face it crosses within REACH_M lies across them
        const F64 lx0 = std::min(x - rx * REACH_M, x + rx * REACH_M) - 1e-6, lx1 = std::max(x - rx * REACH_M, x + rx * REACH_M) + 1e-6;
        const F64 ly0 = std::min(y - ry * REACH_M, y + ry * REACH_M) - 1e-6, ly1 = std::max(y - ry * REACH_M, y + ry * REACH_M) + 1e-6;
        static std::vector<U32> marks;          // by segment: the call that took it last
        static U32 call = 0;
        if (marks.size() < sSegments.size())
        {
            marks.resize(sSegments.size(), 0);
        }
        ++call;
        sSpans.clear();
        sSpanZ.clear();
        auto take = [&](S32 k)
        {
            if (marks[k] == call)
            {
                return;
            }
            marks[k] = call;
            for (const PieceShapes* p : sActiveRoots[k]->pieces)
            {
                for (const Shape& sh : p->shapes)
                {
                    if (sh.x1 < lx0 || sh.x0 > lx1 || sh.y1 < ly0 || sh.y0 > ly1 || std::fabs(sh.z - z) > Z_REACH)
                    {
                        continue;
                    }
                    addSpan(sh, x, y, rx, ry);
                }
            }
        };
        for (S32 k : segments)
        {
            take(k);
            for (S32 h : siblings(k))
            {
                take(h);
            }
        }
        return mergeSpans(left, right, zroad);
    }

    // The way into a slab (a square piece, or one entered across): toward its centre from this piece's (a
    // slab's) or from where the line leaves it, so a run of small unlinked pieces, each a slab, steers along
    // the line of the pieces where the road bends, rather than carrying the first heading on for ever.
    void slabWay(const Segment& from, bool from_slab, F64 ex, F64 ey, const Segment& to, F64 dx, F64 dy, F64& ox, F64& oy)
    {
        const F64 sx = from_slab ? from.cx : ex, sy = from_slab ? from.cy : ey;
        const F64 L = std::hypot(to.cx - sx, to.cy - sy);
        if (L < 1.0)
        {
            ox = dx;
            oy = dy;
            return;
        }
        ox = (to.cx - sx) / L;
        oy = (to.cy - sy) / L;
    }

    constexpr F64 MAX_BRANCH_TURN_DEG = 120.0;
    constexpr F64 SIDE_ENTRY_DEG = 60.0;        // a piece entered further off the way than this, by its side,
    constexpr F64 ALONG_ENTRY_DEG = 30.0;       // and running within this of the way, is gone along
    constexpr F64 UNDERLAP_M = 0.1;             // a piece this much lower where it meets one is under it
    constexpr F64 ROUTE_NEAR_M = 6.0;           // the route's piece nearest the car, within this, is where it is
    constexpr F64 JOIN_MIN_WAY_M = 2.0;         // a way on shares at least this much edge (a car's width or so)  // sharper is back the way we came
    constexpr F64 SAME_BRANCH_DEG = 20.0;       // ways on closer in direction are one branch
    constexpr F64 MIN_BRANCH_W = 3.0;           // narrower linksets are not a road on (a sign, a post)
    // A turn off the way (TURN_NARROW_DEG or more) is onto a part the car fits with LANE_MARGIN_M either side: a
    // cobbled footpath (Leafminer) or a driveway (Shetlon) 3-4 m across is no road to turn onto.
    constexpr F64 TURN_NARROW_DEG = 45.0;
    F64 turnMinBranchW() { return 2 * (sCarHalfW + LANE_MARGIN_M); }
    constexpr F64 LONE_PIECE_M = 12.0;          // a side piece shorter than this, meeting nothing further, is no branch
    constexpr F64 MAX_BRANCH_DZ = 1.5;          // nor ones higher or lower than a car drives onto

    F64 squareRadius(const Segment& sg)
    {
        F64 r = 0;
        for (const auto& [x, y] : sg.pts)
        {
            r = std::max(r, std::hypot(x - sg.cx, y - sg.cy));
        }
        return r;
    }

    // How well a way on carries the road on: straighter, and in the same surface.
    F64 branchScore(const Branch& b)
    {
        return std::cos(b.turn_deg * PI / 180) + (b.same_surface ? SAME_SURFACE_BONUS : 0.0) + (b.on_route ? ROUTE_BONUS : 0.0)
               - (b.definite ? 0.0 : UNDECIDED_PENALTY);
    }

    // Choices made, by the linkset arrived on, kept a while so that the path, worked out again every half
    // second, does not change its mind at the junction.
    struct Choice
    {
        LLUUID to;
        F64 when = 0;
    };
    std::unordered_map<LLUUID, Choice> sChoices;
    constexpr F64 CHOICE_KEEP_S = 60.0;

    // The junction the walk last committed to, by where it is and the way taken: held until the car has passed it,
    // whatever part the walk meets it from, so the line never turns into a branch one walk and carries straight on
    // the next (a left into a side road, then straight on past it).
    struct JunctionCommit
    {
        bool valid = false;
        F64 x = 0, y = 0;           // the junction
        F64 dx = 1, dy = 0;         // the way taken out of it
        F64 turn_deg = 0;
        LLUUID id, root;
    };
    JunctionCommit sCommit;
    constexpr F64 COMMIT_NEAR_M = 15.0, COMMIT_DIR_DEG = 35.0, COMMIT_PASSED_M = 10.0, COMMIT_FORGET_M = 120.0;

    // The committed branch among a junction's ways: the same part, else the same linkset, else the same way out.
    S32 committedBranch(const std::vector<Branch>& branches)
    {
        for (size_t i = 0; i < branches.size(); ++i)
        {
            if (branches[i].id() == sCommit.id)
            {
                return S32(i);
            }
        }
        S32 best = -1;
        F64 best_cos = std::cos(COMMIT_DIR_DEG * PI / 180);
        for (size_t i = 0; i < branches.size(); ++i)
        {
            const F64 c = branches[i].dx * sCommit.dx + branches[i].dy * sCommit.dy + (branches[i].root == sCommit.root ? 0.05 : 0.0);
            if (c > best_cos)
            {
                best_cos = c;
                best = S32(i);
            }
        }
        return best;
    }

    // The commitment let go once the car is past the junction along the way taken, or far from it.
    void forgetPassedCommit(F64 cx, F64 cy)
    {
        if (!sCommit.valid)
        {
            return;
        }
        const F64 past = (cx - sCommit.x) * sCommit.dx + (cy - sCommit.y) * sCommit.dy;
        const F64 dist = std::hypot(cx - sCommit.x, cy - sCommit.y);
        if (past > COMMIT_PASSED_M || dist > COMMIT_FORGET_M)
        {
            LL_INFOS("SKRoadDriver") << "junction at " << S32(sCommit.x) << ", " << S32(sCommit.y) << " passed: commitment ("
                                     << S32(sCommit.turn_deg) << " deg) released" << LL_ENDL;
            sCommit = JunctionCommit();
        }
    }

    // ---------------------------------------------------------------- the roads driven

    // Every DRIVEN_CELL_M square the car has been driven through (the autopilot or Nexii by hand), kept across
    // sessions in sk-road-driven.json (logs folder: {"cell_m": 10, "cells": [[x, y], ...]}, global cells), read at
    // the first need, added to as the car drives, written every DRIVEN_SAVE_S and when the autopilot stops.
    constexpr F64 DRIVEN_CELL_M = 10.0, DRIVEN_SAVE_S = 30.0;
    std::unordered_set<U64> sDrivenCells;
    std::unordered_set<LLUUID> sDrivenRoots;    // the road linksets the car has driven on: the roads themselves
    bool sDrivenRead = false, sDrivenDirty = false;
    F64 sDrivenSaved = 0;

    U64 drivenKey(S64 cx, S64 cy) { return (U64(U32(S32(cx))) << 32) | U32(S32(cy)); }
    std::string drivenPath() { return host::logPath("sk-road-driven.json"); }

    void readDriven()
    {
        if (sDrivenRead)
        {
            return;
        }
        sDrivenRead = true;
        llifstream in(drivenPath().c_str());
        std::stringstream text;
        text << in.rdbuf();
        LLSD d;
        if (in.is_open() && LlsdFromJsonString(text.str(), d) && d["cells"].isArray())
        {
            for (const LLSD& c : llsd::inArray(d["cells"]))
            {
                sDrivenCells.insert(drivenKey(c[0].asInteger(), c[1].asInteger()));
            }
            for (const LLSD& r : llsd::inArray(d["roots"]))
            {
                sDrivenRoots.insert(r.asUUID());
            }
        }
    }

    void writeDriven()
    {
        if (!sDrivenDirty)
        {
            return;
        }
        sDrivenDirty = false;
        llofstream out(drivenPath().c_str(), std::ios::out | std::ios::trunc);
        out << "{\"cell_m\": " << S32(DRIVEN_CELL_M) << ", \"cells\": [";
        bool first = true;
        for (U64 k : sDrivenCells)
        {
            out << (first ? "" : ",") << '[' << S32(U32(k >> 32)) << ',' << S32(U32(k & 0xffffffffu)) << ']';
            first = false;
        }
        out << "], \"roots\": [";
        first = true;
        for (const LLUUID& r : sDrivenRoots)
        {
            out << (first ? "" : ",") << '"' << r << '"';
            first = false;
        }
        out << "]}\n";
    }

    void noteDriven(const LLVector3d& pos, F64 now)
    {
        readDriven();
        // the road under the car: the first linkset of the walk's chain, the one the car is on
        if (!sChainRoots.empty() && sChainRoots.front().notNull() && sDrivenRoots.insert(sChainRoots.front()).second)
        {
            sDrivenDirty = true;
        }
        if (sDrivenCells.insert(drivenKey(S64(std::floor(pos.mdV[VX] / DRIVEN_CELL_M)), S64(std::floor(pos.mdV[VY] / DRIVEN_CELL_M)))).second)
        {
            sDrivenDirty = true;
        }
        if (now - sDrivenSaved > DRIVEN_SAVE_S)
        {
            sDrivenSaved = now;
            writeDriven();
        }
    }

    bool driven(F64 x, F64 y)
    {
        readDriven();
        return sDrivenCells.count(drivenKey(S64(std::floor(x / DRIVEN_CELL_M)), S64(std::floor(y / DRIVEN_CELL_M)))) > 0;
    }

    // The share of the way a branch leads (EXPLORE_LOOK_M along its direction from where it starts) not yet
    // driven, 0 to 1.
    constexpr F64 EXPLORE_LOOK_M = 50.0, EXPLORE_W = 2.0;
    F64 undrivenAhead(const Branch& b)
    {
        readDriven();
        // the road it leads onto, never driven: wholly new (exact, the linkset the branch is); driven, the cells
        // along its way say how much of it (a whole road in one linkset is driven once any of it is)
        if (b.root.notNull() && !sDrivenRoots.count(b.root))
        {
            return 1.0;
        }
        S32 n = 0, fresh = 0;
        for (F64 d = 5.0; d <= EXPLORE_LOOK_M; d += 5.0)
        {
            ++n;
            fresh += driven(b.x + b.dx * d, b.y + b.dy * d) ? 0 : 1;
        }
        return n ? F64(fresh) / n : 0.0;
    }

    // The branch to take at a junction (its branches best first). The policy for now: carry the road on, and with
    // no way straight on, explore's choice or to the right
    // (straightest, same surface). This is the one place a later one goes: a road not yet driven, or Linden,
    // mole or private roads by the branch's linkset's owner and group.
    BranchPicker sBranchPicker;
    void setBranchPicker(BranchPicker picker)
    {
        sBranchPicker = std::move(picker);
    }

    S32 chooseBranch(const Junction& jn)
    {
        const F64 now = sClock.getElapsedTimeF64();
        auto kept = sChoices.find(jn.from);
        if (kept != sChoices.end() && now - kept->second.when < CHOICE_KEEP_S)
        {
            for (size_t i = 0; i < jn.branches.size(); ++i)
            {
                if (jn.branches[i].id() == kept->second.to)
                {
                    kept->second.when = now;
                    return S32(i);
                }
            }
        }
        // SKRoadAutopilotGoal "explore" (the default): the way on, weighed with how much of where each branch
        // leads has not been driven (EXPLORE_W against carrying the road on); "carry": the road carried on
        const std::string goal = host::goal();
        S32 pick = 0;
        // "route": the committed route's branch; with no plan, or none matching it, explore's choice
        S32 routed = -1;
        if (goal == "route")
        {
            std::vector<double> bearings;
            for (const Branch& b : jn.branches)
            {
                const double deg = std::atan2(b.dx, b.dy) * 180.0 / PI;
                bearings.push_back(deg < 0 ? deg + 360.0 : deg);
            }
            routed = sBranchPicker ? sBranchPicker(jn.x, jn.y, bearings) : -1;
        }
        if (routed >= 0)
        {
            pick = routed;
        }
        else if (goal == "explore" || goal == "route")
        {
            F64 best = -1e9;
            for (size_t i = 0; i < jn.branches.size(); ++i)
            {
                const F64 sc = branchScore(jn.branches[i]) + EXPLORE_W * undrivenAhead(jn.branches[i]);
                if (sc > best)
                {
                    best = sc;
                    pick = S32(i);
                }
            }
        }
        else if (std::none_of(jn.branches.begin(), jn.branches.end(), [](const Branch& b) { return std::fabs(b.turn_deg) < 30.0; }))
        {
            // no way straight on (a T): to the right, as a cruise keeps to it
            for (size_t i = 1; i < jn.branches.size(); ++i)
            {
                if (jn.branches[i].turn_deg < jn.branches[pick].turn_deg)
                {
                    pick = S32(i);
                }
            }
        }
        sChoices[jn.from] = { jn.branches[pick].id(), now };
        std::string ways;
        for (const Branch& b : jn.branches)
        {
            ways += (ways.empty() ? "" : ", ") + std::string(b.kind());
        }
        LL_INFOS("SKRoadDriver") << "junction ahead (" << ways << "): taking " << jn.branches[pick].kind() << LL_ENDL;
        return pick;
    }

    // The lane line across a square segment (a junction slab), from where it enters to where the chosen
    // way on leaves: straight on, or round the corner where the way in and the way out cross.
    void drawAcross(std::vector<LanePoint>& out, F64& travelled, const Segment& sg, F64 px, F64 py, F64 dx, F64 dy,
                    F64 qx, F64 qy, F64 qdx, F64 qdy, F64 shift)
    {
        std::vector<std::pair<F64, F64>> corners{ { px, py } };
        const F64 den = dx * qdy - dy * qdx;
        if (std::fabs(den) > 0.2)
        {
            // where px + t d meets q - u qd
            const F64 t = ((qx - px) * qdy - (qy - py) * qdx) / den;
            if (t > 0)
            {
                corners.push_back({ px + t * dx, py + t * dy });
            }
        }
        corners.push_back({ qx, qy });
        for (size_t c = 0; c + 1 < corners.size(); ++c)
        {
            const F64 sx = corners[c].first, sy = corners[c].second;
            const F64 L = std::hypot(corners[c + 1].first - sx, corners[c + 1].second - sy);
            if (L < 1e-3)
            {
                continue;
            }
            const F64 ux = (corners[c + 1].first - sx) / L, uy = (corners[c + 1].second - sy) / L;
            const F64 rx = uy, ry = -ux;
            for (F64 s_ = 0; s_ < L; s_ += PATH_STEP)
            {
                LanePoint lp;
                lp.x = sx + s_ * ux + (c == 0 ? 0.0 : shift) * rx;
                lp.y = sy + s_ * uy + (c == 0 ? 0.0 : shift) * ry;
                lp.ax = ux;
                lp.ay = uy;
                lp.lane_w = out.empty() ? sg.width / 2 : out.back().lane_w;
                lp.road_w = out.empty() ? sg.width : out.back().road_w;
                lp.lanes = out.empty() ? 2 : out.back().lanes;
                lp.z = zAt(sg, lp.x, lp.y);
                lp.left_x = lp.x - 1.0 * rx;
                lp.left_y = lp.y - 1.0 * ry;
                lp.right_x = lp.x + 1.0 * rx;
                lp.right_y = lp.y + 1.0 * ry;
                out.push_back(lp);
                travelled += PATH_STEP;
            }
        }
    }

    // ---------------------------------------------------------------- the road, simply

    // The road is its Linden pieces (definite: an LDPW or Maintenance group, a Linden's or mole's, a road
    // name; a piece whose properties have not come waits, a private one never counts) and how their top faces
    // meet, edge to edge (sJoins). A join across the road is an edge across it, so its middle lies on the
    // road's centre line (on a road built in halves, on our half's), and its length is the road's width
    // there. The line runs from join to join along the joined pieces, through a curved piece's parts; where
    // more than one join leads on it is a junction (chooseBranch).

    // The definite piece under the car, or the nearest along the way it faces within ROUTE_NEAR_M.
    S32 definiteStart(F64 x, F64 y, F64 z, F64 hx, F64 hy)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - definiteStart");
        S32 pending_under = -1;
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it != sHash.end())
        {
            for (S32 i : it->second)
            {
                const Shape& sh = *sShapes[i];
                if (sh.piece && std::fabs(heightAt(sh, x, y) - z) < 2.5 && inside(sh, x, y))
                {
                    auto f = sSegByRoot.find(sh.piece->root);
                    if (f != sSegByRoot.end() && sSegments[f->second].width >= MIN_BRANCH_W)
                    {
                        if (sSegments[f->second].definite)
                        {
                            return f->second;   // (not a centre-line strip or a catseye laid on the road)
                        }
                        if (sSegments[f->second].pending && pending_under < 0)
                        {
                            pending_under = f->second;
                        }
                    }
                }
            }
        }
        S32 best = -1;
        F64 best_d = ROUTE_NEAR_M;
        const bool route_fresh = !sRoute.empty() && sClock.getElapsedTimeF64() - sRouteTime < ROUTE_KEEP_S;
        for (S32 k = 0; k < S32(sSegments.size()); ++k)
        {
            const Segment& g = sSegments[k];
            if (!g.definite || g.width < MIN_BRANCH_W || std::fabs(zAt(g, x, y) - z) > Z_REACH
                || (!g.square && std::fabs(g.ax * hx + g.ay * hy) < 0.5))
            {
                continue;
            }
            const F64 a = std::clamp((x - g.cx) * g.ax + (y - g.cy) * g.ay, g.lo, g.hi);
            F64 d = std::max(0.0, std::hypot(x - (g.cx + a * g.ax), y - (g.cy + a * g.ay)) - g.width / 2);
            if (route_fresh && sRoute.count(g.root))
            {
                d -= 1.0;       // the road it was on
            }
            if (d < best_d)
            {
                best_d = d;
                best = k;
            }
        }
        if (best < 0 && pending_under >= 0)
        {
            // on a piece whose name has not come, with no known road near: it, meanwhile, asked for first
            if (sAskFirstSet.insert(sSegments[pending_under].root).second)
            {
                sAskFirst.push_back(sSegments[pending_under].root);
            }
            return pending_under;
        }
        return best;
    }

    // Is the piece one half of a road built in halves: a Linden road piece alongside it, a real lane's width
    // (not a centre-line strip, a catseye or a kerb laid as a linkset of its own) and at least half its own?
    bool halfAlongside(S32 idx)
    {
        for (S32 k : siblings(idx))
        {
            const Segment& o = sSegments[k];
            if (o.definite && o.width >= MIN_BRANCH_W && o.width >= 0.5 * sSegments[idx].width)
            {
                return true;
            }
        }
        return false;
    }

    // The road's width at a point, across the way (dx, dy): a line across it clipped to the top faces of the
    // given pieces and the halves alongside them. Where it misses, -1.
    F64 widthAcross(F64 x, F64 y, F64 z, F64 dx, F64 dy, std::initializer_list<S32> pieces)
    {
        F64 left, right, zr;
        return sectionOn(x, y, z, dx, dy, pieces, left, right, zr) ? right - left : -1.0;
    }

    // How much top edge two pieces share: the road's width where they meet.
    F64 joinLength(S32 a, S32 b)
    {
        for (const Join& j : sJoins[a])
        {
            if (j.other == b)
            {
                // the extent of what they share across the road, not the sum of every edge (an overlap of
                // ends shares several)
                return std::max(2.0, j.hi > j.lo ? j.hi - j.lo : j.length);
            }
        }
        return std::max(2.0, sSegments[b].width);
    }

    std::vector<LanePoint> joinPath(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 v, F64 keep)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - joinPath");
        std::vector<LanePoint> out;
        S32 cur = definiteStart(x, y, z, hx, hy);
        if (cur < 0)
        {
            return out;
        }
        struct Centre
        {
            F64 x, y, w;
            bool half;      // on a piece with a half alongside: the centre is already our lane's
            S32 a, b;       // the pieces it lies on (the same one, or the two a join is between)
        };
        std::vector<Centre> line;
        const F64 length = lookAhead(v) + 10.0;
        std::unordered_set<S32> visited{ cur };
        sChainRoots.assign(1, sSegments[cur].root);
        F64 dx = hx, dy = hy;
        // the first centre: abreast of the car on the piece's centre line where it has one
        F64 px = x, py = y;
        {
            const Segment& g = sSegments[cur];
            if (!g.square)
            {
                const bool flip = g.ax * dx + g.ay * dy < 0;
                dx = flip ? -g.ax : g.ax;
                dy = flip ? -g.ay : g.ay;
                const F64 a = (x - g.cx) * g.ax + (y - g.cy) * g.ay;
                px = g.cx + a * g.ax;
                py = g.cy + a * g.ay;
            }
            const bool half0 = halfAlongside(cur);
            F64 w0 = half0 ? g.width : widthAcross(px, py, z, dx, dy, { cur });
            line.push_back({ px, py, w0 > 0.5 ? w0 : g.width, half0, cur, cur });
        }
        F64 travelled = 0;
        while (travelled < length)
        {
            const Segment& sg = sSegments[cur];
            std::vector<Branch> ways;
            for (const Join& jn : sJoins[cur])
            {
                if (visited.count(jn.other) || jn.length < JOIN_MIN_WAY_M)
                {
                    continue;
                }
                const Segment& nx = sSegments[jn.other];
                if ((!nx.definite && !nx.pending) || nx.width < MIN_BRANCH_W || ((jn.x - px) * dx + (jn.y - py) * dy) < 0.5)
                {
                    continue;       // not Linden road (or a centre-line strip, a catseye), or behind
                }
                if (nx.pending)
                {
                    // joined to the road ahead and its name not here yet: it carries the road on meanwhile (at
                    // speed the car outruns the replies), asked for before anything else
                    if (sAskFirstSet.insert(nx.root).second)
                    {
                        sAskFirst.push_back(nx.root);
                    }
                }
                const bool along = std::fabs(jn.ex * dx + jn.ey * dy) > 0.7;     // the edge runs the way we go
                if (along && !nx.square && std::fabs(nx.ax * dx + nx.ay * dy) > std::cos(SIBLING_DEG * PI / 180))
                {
                    continue;       // the road's other half, alongside
                }
                Branch b;
                b.seg = jn.other;
                b.root = nx.root;
                b.same_surface = nx.matches;
                b.definite = nx.definite;      // a pending one scores under a known one (UNDECIDED_PENALTY)
                b.on_route = sRoute.count(nx.root) > 0;
                b.x = jn.x;
                b.y = jn.y;
                // on into it: along its axis away from the join where it is long, toward its centre otherwise
                if (!nx.square && std::fabs(nx.ax * dx + nx.ay * dy) >= 0.5)
                {
                    const bool away = (nx.cx - jn.x) * nx.ax + (nx.cy - jn.y) * nx.ay >= 0;
                    b.dx = away ? nx.ax : -nx.ax;
                    b.dy = away ? nx.ay : -nx.ay;
                }
                else
                {
                    slabWay(sg, true, jn.x, jn.y, nx, dx, dy, b.dx, b.dy);
                }
                b.turn_deg = std::atan2(dx * b.dy - dy * b.dx, b.dx * dx + b.dy * dy) * 180 / PI;
                if (std::fabs(b.turn_deg) > MAX_BRANCH_TURN_DEG)
                {
                    continue;
                }
                ways.push_back(b);
            }
            std::sort(ways.begin(), ways.end(), [](const Branch& a, const Branch& b) { return branchScore(a) > branchScore(b); });
            auto aside = [&](const Branch& b) { return std::fabs(-(b.x - px) * dy + (b.y - py) * dx); };
            std::vector<Branch> branches;
            for (const Branch& b : ways)
            {
                auto same = std::find_if(branches.begin(), branches.end(),
                                         [&b](const Branch& o) { return std::fabs(o.turn_deg - b.turn_deg) < SAME_BRANCH_DEG; });
                if (same == branches.end())
                {
                    branches.push_back(b);
                }
                else if (aside(b) < aside(*same))
                {
                    *same = b;
                }
            }
            // a road does not branch into itself: the linkset the car is on, met again across it (a region
            // crossing read the road's own halves as a crossroads, right and left both itself), is no way on
            branches.erase(std::remove_if(branches.begin(), branches.end(),
                                          [&](const Branch& b) { return std::fabs(b.turn_deg) > 45.0 && (b.root == sg.root || std::find(sChainRoots.begin(), sChainRoots.end(), b.root) != sChainRoots.end()); }),
                           branches.end());
            if (branches.empty())
            {
                // the road ends here (or its next pieces have not come yet): on to the piece's far side
                const F64 through = reach(sg, px, py, dx, dy);
                line.push_back({ px + through * dx, py + through * dy, line.back().w, line.back().half, cur, cur });
                break;
            }
            S32 pick = 0;
            if (branches.size() > 1)
            {
                Junction j;
                j.from = sg.root;
                j.x = px;
                j.y = py;
                j.branches = branches;
                pick = chooseBranch(j);
                j.chosen = pick;
                sView.junctions.push_back(std::move(j));
            }
            const Branch go = branches[pick];
            // a curved piece: its parts' middles between where the line came in and the join it leaves by
            if (!sg.straight && !sg.square)
            {
                const F64 cx = go.x - px, cy = go.y - py, L = std::hypot(cx, cy);
                std::vector<std::pair<F64, std::pair<F64, F64>>> mids;
                for (const auto& [mx, my] : sg.part_mids)
                {
                    const F64 t = L > 1e-3 ? ((mx - px) * cx + (my - py) * cy) / (L * L) : -1;
                    if (t > 0.05 && t < 0.95)
                    {
                        mids.push_back({ t, { mx, my } });
                    }
                }
                std::sort(mids.begin(), mids.end());
                for (const auto& m : mids)
                {
                    line.push_back({ m.second.first, m.second.second, line.back().w, line.back().half, cur, cur });
                }
            }
            const bool half = halfAlongside(go.seg);
            Branch at_join = go;
            {
                // the road's width there, measured across it on the two pieces' own faces (a half's own width
                // where the road is built in halves)
                const F64 jz = zAt(sSegments[go.seg], at_join.x, at_join.y);
                F64 w = half ? sSegments[go.seg].width : widthAcross(at_join.x, at_join.y, jz, go.dx, go.dy, { cur, go.seg });
                if (w <= 0.5)
                {
                    w = half ? sSegments[go.seg].width : joinLength(cur, go.seg);
                }
                line.push_back({ at_join.x, at_join.y, w, half, cur, go.seg });
            }
            travelled += std::hypot(go.x - px, go.y - py);
            visited.insert(go.seg);
            sChainRoots.push_back(go.root);
            cur = go.seg;
            px = go.x;
            py = go.y;
            dx = go.dx;
            dy = go.dy;
        }
        // the lane: right of the centre line by a quarter of the road on a two-lane piece, on a half where it is
        for (size_t i = 0; i < line.size(); ++i)
        {
            const size_t a = i > 0 ? i - 1 : i, b = i + 1 < line.size() ? i + 1 : i;
            F64 ux = line[b].x - line[a].x, uy = line[b].y - line[a].y;
            const F64 L = std::hypot(ux, uy);
            if (L < 1e-6)
            {
                ux = dx;
                uy = dy;
            }
            else
            {
                ux /= L;
                uy /= L;
            }
            // The road across here, from its own faces: a line square to the way clipped to the top faces of the
            // pieces the point lies on (and their halves). Its ends are the road's edges (the overlay's blue
            // line), its middle the road's centre, its length the width. Only where it misses, the estimate.
            F64 cx = line[i].x, cy = line[i].y, w = line[i].w;
            bool measured = false;
            {
                F64 left, right, zr;
                const F64 pz = zAt(sSegments[line[i].a], line[i].x, line[i].y);
                if (sectionOn(line[i].x, line[i].y, pz, ux, uy, { line[i].a, line[i].b }, left, right, zr) && right - left > 0.5)
                {
                    const F64 mid = (left + right) / 2;
                    cx = line[i].x + uy * mid;      // section's offsets are to the right: (uy, -ux)
                    cy = line[i].y - ux * mid;
                    w = right - left;
                    measured = true;
                }
            }
            if (!measured)
            {
                ++sView.width_fallbacks;
            }
            // on a half, the centre is our lane's already: back to the measured middle only on a whole road
            if (line[i].half)
            {
                cx = line[i].x;
                cy = line[i].y;
            }
            const S32 lanes = (!line[i].half && w >= TWO_LANES_M) ? 2 : 1;
            const F64 shift = lanes == 2 ? laneShiftIn(w, keep) : 0.0;
            LanePoint lp;
            lp.x = cx + uy * shift;
            lp.y = cy - ux * shift;
            lp.ax = ux;
            lp.ay = uy;
            lp.lane_w = w / lanes;
            lp.road_w = w;
            lp.lanes = lanes;
            lp.z = z;
            lp.left_x = cx - uy * w / 2;
            lp.left_y = cy + ux * w / 2;
            lp.right_x = cx + uy * w / 2;
            lp.right_y = cy - ux * w / 2;
            out.push_back(lp);
        }
        // every PATH_STEP along it, as the controller reads it
        std::vector<LanePoint> dense;
        for (size_t i = 0; i + 1 < out.size(); ++i)
        {
            const LanePoint& a = out[i];
            const LanePoint& b = out[i + 1];
            const F64 L = std::hypot(b.x - a.x, b.y - a.y);
            const S32 n = std::max(1, S32(std::ceil(L / PATH_STEP)));
            for (S32 k = 0; k < n; ++k)
            {
                const F64 t = F64(k) / n;
                LanePoint q = a;
                q.x = a.x + (b.x - a.x) * t;
                q.y = a.y + (b.y - a.y) * t;
                q.left_x = a.left_x + (b.left_x - a.left_x) * t;
                q.left_y = a.left_y + (b.left_y - a.left_y) * t;
                q.right_x = a.right_x + (b.right_x - a.right_x) * t;
                q.right_y = a.right_y + (b.right_y - a.right_y) * t;
                if (L > 1e-6)
                {
                    q.ax = (b.x - a.x) / L;
                    q.ay = (b.y - a.y) / L;
                }
                dense.push_back(q);
            }
        }
        if (!out.empty())
        {
            dense.push_back(out.back());
        }
        // the controller wants the line from the car on: drop what lies behind it
        size_t first = 0;
        while (first + 1 < dense.size() && (dense[first + 1].x - x) * dense[first + 1].ax + (dense[first + 1].y - y) * dense[first + 1].ay < 0)
        {
            ++first;
        }
        dense.erase(dense.begin(), dense.begin() + first);
        return dense;
    }

    // ---------------------------------------------------------------- the road as boxes laid end to end

    // A road piece is a part, linked or not: the road is boxes laid end to end. A part's top (a plain box's top
    // face, or its upward sides' outline standing in for one) has its edges; where an edge of one part lies on
    // an edge of the next, the road goes on from one to the other. The walk goes from the part under the car
    // along it to its far end, onto the part meeting it there, and so on; where more than one meets it there it
    // is a junction (chooseBranch).
    struct Meet
    {
        S32 edge_a = -1, edge_b = -1;           // the two parts' top edges that meet
        F64 x = 0, y = 0, z = 0;                // the middle of what they share
        F64 ax = 0, ay = 0, bx = 0, by = 0;     // its ends
        F64 overlap = 0;
    };
    struct PartPair
    {
        U64 a = 0, b = 0;                       // the two parts' keys (pieceKey), the lower id's first
        std::vector<Meet> meets;
        F64 used = 0;
    };
    std::unordered_map<U64, PartPair> sPartPairs;   // settled once, as the joins are
    std::vector<PieceShapes*> sParts;           // the road's parts in reach
    std::vector<U8> sPartDefinite, sPartPending;    // their linksets' verdicts (see Segment)
    std::vector<std::vector<Continuation>> sConts;  // by part
    std::unordered_map<const PieceShapes*, S32> sPartIndex;
    std::vector<S32> sWalkParts;                // the parts the last walk went through, for the overlay
    constexpr F64 MEET_INLINE_M = 0.05;         // an end line this near the other's line lies on it,
    constexpr F64 MEET_DZ_M = 0.15;             // this near in height,
    constexpr F64 MEET_SHARE = 0.5;             // across at least this share of the narrower end,
    constexpr F64 MEET_COS = 0.866;             // facing it within 30 degrees,
    constexpr F64 MEET_BEYOND_M = 0.5;          // and each going on this far past the other's

    bool onTop(const PieceShapes& p, F64 x, F64 y, F64 z)
    {
        for (const Shape& sh : p.shapes)
        {
            if (x >= sh.tx0 && x <= sh.tx1 && y >= sh.ty0 && y <= sh.ty1 && inside(sh, x, y) && std::fabs(heightAt(sh, x, y) - z) <= MEET_DZ_M)
            {
                return true;
            }
        }
        return false;
    }

    // Does b's top edge eb meet a's ea: on it end to end (in line, level, across most of the narrower, facing
    // it), or the two ends laid over each other (each lying on the other's top, a kit's pieces overlapped)?
    bool meetEdges(const PieceShapes& a, S32 ia, const PieceShapes& b, S32 ib, Meet& m)
    {
        const TopEdge& ea = a.top[ia];
        const TopEdge& eb = b.top[ib];
        if (ea.ox * eb.ox + ea.oy * eb.oy > -MEET_COS)
        {
            return false;
        }
        const F64 La = std::hypot(ea.x1 - ea.x0, ea.y1 - ea.y0), Lb = std::hypot(eb.x1 - eb.x0, eb.y1 - eb.y0);
        if (La < 1e-3 || Lb < 1e-3)
        {
            return false;
        }
        const F64 tx = (ea.x1 - ea.x0) / La, ty = (ea.y1 - ea.y0) / La;
        const F64 s0 = (eb.x0 - ea.x0) * tx + (eb.y0 - ea.y0) * ty, s1 = (eb.x1 - ea.x0) * tx + (eb.y1 - ea.y0) * ty;
        const F64 lo = std::max(0.0, std::min(s0, s1)), hi = std::min(La, std::max(s0, s1));
        // the parts' widths across the way through the edges (a bevelled kerb's short edges are not an end)
        auto across = [](const PieceShapes& p, F64 ox, F64 oy)
        {
            return std::fabs(p.ux * ox + p.uy * oy) >= std::fabs(-p.uy * ox + p.ux * oy) ? p.lv : p.lu;
        };
        if (hi - lo < MEET_SHARE * std::min(across(a, ea.ox, ea.oy), across(b, eb.ox, eb.oy)) || std::fabs(s1 - s0) < 1e-6)
        {
            return false;
        }
        // each goes on past the other's edge: the next piece, not one beside or over it
        auto beyond = [](const PieceShapes& p, const TopEdge& e)
        {
            F64 most = -1e18;
            for (const TopEdge& t : p.top)
            {
                most = std::max(most, std::max((t.x0 - e.x0) * e.ox + (t.y0 - e.y0) * e.oy, (t.x1 - e.x0) * e.ox + (t.y1 - e.y0) * e.oy));
            }
            return most;
        };
        if (beyond(b, ea) < MEET_BEYOND_M || beyond(a, eb) < MEET_BEYOND_M)
        {
            return false;
        }
        // eb's point abreast of s along ea, and both edges' heights there
        auto bAt = [&](F64 s, F64& x, F64& y, F64& z)
        {
            const F64 t = (s - s0) / (s1 - s0);
            x = eb.x0 + (eb.x1 - eb.x0) * t;
            y = eb.y0 + (eb.y1 - eb.y0) * t;
            z = eb.z0 + (eb.z1 - eb.z0) * t;
        };
        F64 blx, bly, blz, bhx, bhy, bhz, bmx, bmy, bmz;
        bAt(lo, blx, bly, blz);
        bAt(hi, bhx, bhy, bhz);
        const F64 sm = (lo + hi) / 2;
        bAt(sm, bmx, bmy, bmz);
        const F64 off_lo = (blx - ea.x0) * ea.ox + (bly - ea.y0) * ea.oy, off_hi = (bhx - ea.x0) * ea.ox + (bhy - ea.y0) * ea.oy;
        const F64 amx = ea.x0 + tx * sm, amy = ea.y0 + ty * sm, amz = ea.z0 + (ea.z1 - ea.z0) * sm / La;
        if (std::fabs(off_lo) <= MEET_INLINE_M && std::fabs(off_hi) <= MEET_INLINE_M)
        {
            if (std::fabs(amz - bmz) > MEET_DZ_M)
            {
                return false;
            }
            m.x = amx;
            m.y = amy;
        }
        else if (off_lo < -MEET_INLINE_M && off_hi < -MEET_INLINE_M)
        {
            if (!onTop(a, bmx, bmy, bmz) || !onTop(b, amx, amy, amz))
            {
                return false;
            }
            m.x = (amx + bmx) / 2;
            m.y = (amy + bmy) / 2;
        }
        else
        {
            return false;       // a gap between them, or one across the other
        }
        m.z = (amz + bmz) / 2;
        m.edge_a = ia;
        m.edge_b = ib;
        m.overlap = hi - lo;
        m.ax = ea.x0 + tx * lo;
        m.ay = ea.y0 + ty * lo;
        m.bx = ea.x0 + tx * hi;
        m.by = ea.y0 + ty * hi;
        return true;
    }

    // Where two parts meet (a's id the lower), from the cache while both are as they were: the best meeting
    // at each of a's edges.
    bool overlapMeet(const PieceShapes& a, const PieceShapes& b, Meet& m);

    const PartPair& partPair(const PieceShapes& a, const PieceShapes& b, F64 now)
    {
        const U64 ka = pieceKey(a), kb = pieceKey(b);
        PartPair& p = sPartPairs[mix(a.id) ^ (b.id * 0x9E3779B97F4A7C15ULL)];
        if (p.a != ka || p.b != kb)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - partPair");
            p.a = ka;
            p.b = kb;
            p.meets.clear();
            for (S32 ia = 0; ia < S32(a.top.size()); ++ia)
            {
                Meet best;
                for (S32 ib = 0; ib < S32(b.top.size()); ++ib)
                {
                    Meet m;
                    if (meetEdges(a, ia, b, ib, m) && m.overlap > best.overlap)
                    {
                        best = m;
                    }
                }
                if (best.edge_a >= 0)
                {
                    p.meets.push_back(best);
                }
            }
            if (p.meets.empty())
            {
                // no edge shared: laid over each other (JP road: at each station several mesh pieces lie on one
                // another, none edge to edge with the next, and the walk stopped after 0 m on every one), each
                // going on past the other: joined where each first lies on the other's top
                Meet m;
                if (overlapMeet(a, b, m))
                {
                    p.meets.push_back(m);
                    Meet back;
                    if (overlapMeet(b, a, back))
                    {
                        std::swap(back.edge_a, back.edge_b);
                        p.meets.push_back(back);
                    }
                }
                else if (overlapMeet(b, a, m))
                {
                    std::swap(m.edge_a, m.edge_b);
                    p.meets.push_back(m);
                }
            }
        }
        p.used = now;
        return p;
    }

    // Two road pieces laid over each other: b's top edge that lies on a's top (its middle within OVERLAP_DZ_M of
    // a's surface there) and faces back into a (the edge a car on a crosses onto b by), with b reaching on past
    // a (its middle off a); a's edge the one facing b. The meeting is that edge of b.
    constexpr F64 OVERLAP_DZ_M = 0.4;
    bool overlapMeet(const PieceShapes& a, const PieceShapes& b, Meet& m)
    {
        if (a.top.empty() || b.top.empty())
        {
            return false;
        }
        auto top_of = [](const PieceShapes& p, F64 x, F64 y, F64& h)
        {
            for (const Shape& sh : p.shapes)
            {
                if (x >= sh.tx0 && x <= sh.tx1 && y >= sh.ty0 && y <= sh.ty1 && inside(sh, x, y))
                {
                    h = heightAt(sh, x, y);
                    return true;
                }
            }
            return false;
        };
        F64 h;
        if (top_of(a, b.cx, b.cy, h) && std::fabs(h - b.zt) < OVERLAP_DZ_M)
        {
            return false;       // b's middle on a: one over the other, not one on from the other
        }
        S32 best_b = -1;
        F64 best_len = 0;
        for (S32 ib = 0; ib < S32(b.top.size()); ++ib)
        {
            const TopEdge& e = b.top[ib];
            const F64 mx = (e.x0 + e.x1) / 2, my = (e.y0 + e.y1) / 2, mz = (e.z0 + e.z1) / 2;
            // facing back toward a's middle, and lying on a's top
            if ((a.cx - mx) * e.ox + (a.cy - my) * e.oy <= 0)
            {
                continue;
            }
            // on a's top at its middle, or within a metre short of it (a seam's ends lie a little askew: an end
            // across the next piece's at an angle, overlapping at one side and short of it at the other)
            bool on = false;
            for (F64 step : { 0.0, 0.5, 1.0 })
            {
                if (top_of(a, mx + e.ox * step, my + e.oy * step, h) && std::fabs(h - mz) <= OVERLAP_DZ_M)
                {
                    on = true;
                    break;
                }
            }
            if (!on)
            {
                continue;
            }
            const F64 len = std::hypot(e.x1 - e.x0, e.y1 - e.y0);
            if (len > best_len)
            {
                best_len = len;
                best_b = ib;
            }
        }
        if (best_b < 0 || best_len < 1.5)
        {
            return false;
        }
        const TopEdge& eb = b.top[best_b];
        S32 best_a = 0;
        F64 best_face = -1e9;
        for (S32 ia = 0; ia < S32(a.top.size()); ++ia)
        {
            const TopEdge& e = a.top[ia];
            const F64 face = e.ox * (b.cx - a.cx) + e.oy * (b.cy - a.cy);
            if (face > best_face)
            {
                best_face = face;
                best_a = ia;
            }
        }
        m.edge_a = best_a;
        m.edge_b = best_b;
        m.x = (eb.x0 + eb.x1) / 2;
        m.y = (eb.y0 + eb.y1) / 2;
        m.z = (eb.z0 + eb.z1) / 2;
        m.ax = eb.x0;
        m.ay = eb.y0;
        m.bx = eb.x1;
        m.by = eb.y1;
        m.overlap = best_len;
        return true;
    }

    // The road's parts in reach and what meets what: each pair worked out once (partPair), the parts near each
    // other found through 16 m cells of their bounds.
    // ---------------------------------------------------------------- the road graph, written down

    // The road as it settles, to sk-road-graph.jsonl in the logs folder (while the survey runs): each road part
    // once, and again when it changes, and each meeting of two parts. One object a line, appended:
    //   {"type":"part","t","id","part","root","definite","box","c":[x,y,z],"axis":[ux,uy],"size":[along,across],
    //    "top_tex","top":[x0,y0,z0,x1,y1,z1,...]}
    //   {"type":"meet","t","a","b","at":[[x,y,z],...]}   a and b the parts' ids (their content keys)
    llofstream sGraphLog;
    std::mutex sGraphLogMutex;                  // the stream, written on the General queue
    std::unordered_set<U64> sGraphParts, sGraphMeets;
    // One line to write: copied on the main thread, formatted and written off it (formatting and writing a
    // drive's new parts took up to 100 ms of a settle with the survey on).
    struct GraphLine
    {
        bool meet = false;
        F64 t = 0;
        U64 a = 0, b = 0;
        LLUUID part, root, top_tex;
        bool definite = false, box = false;
        F64 c[3] = {}, axis[2] = {}, size[2] = {};
        std::vector<F64> nums;                  // the top's edges (6 a edge) or the meetings (3 a meeting)
    };
    std::vector<GraphLine> sGraphPending;
    bool graphOpen()
    {
        if (!host::surveyOn())
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(sGraphLogMutex);
        if (!sGraphLog.is_open())
        {
            sGraphLog.open(host::logPath("sk-road-graph.jsonl").c_str(), std::ios::out | std::ios::app);
            sGraphLog.precision(10);
        }
        return sGraphLog.is_open();
    }
    void writeGraphLines(const std::vector<GraphLine>& lines)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - write graph");
        std::ostringstream out;
        out.precision(10);
        for (const GraphLine& l : lines)
        {
            if (l.meet)
            {
                out << "{\"type\":\"meet\",\"t\":" << l.t << ",\"a\":" << l.a << ",\"b\":" << l.b << ",\"at\":[";
                for (size_t k = 0; k + 2 < l.nums.size(); k += 3)
                {
                    out << (k ? "," : "") << '[' << l.nums[k] << ',' << l.nums[k + 1] << ',' << l.nums[k + 2] << ']';
                }
            }
            else
            {
                out << "{\"type\":\"part\",\"t\":" << l.t << ",\"id\":" << l.a << ",\"part\":\"" << l.part << "\",\"root\":\"" << l.root
                    << "\",\"definite\":" << (l.definite ? "true" : "false") << ",\"box\":" << (l.box ? "true" : "false") << ",\"c\":["
                    << l.c[0] << ',' << l.c[1] << ',' << l.c[2] << "],\"axis\":[" << l.axis[0] << ',' << l.axis[1] << "],\"size\":["
                    << l.size[0] << ',' << l.size[1] << "],\"top_tex\":\"" << l.top_tex << "\",\"top\":[";
                for (size_t k = 0; k < l.nums.size(); ++k)
                {
                    out << (k ? "," : "") << l.nums[k];
                }
            }
            out << "]}\n";
        }
        std::lock_guard<std::mutex> lock(sGraphLogMutex);
        if (sGraphLog.is_open())
        {
            sGraphLog << out.str();
        }
    }
    // Hands what settleParts noted to the General queue (written here where there is none).
    void flushGraph()
    {
        if (sGraphPending.empty())
        {
            return;
        }
        auto lines = std::make_shared<std::vector<GraphLine>>(std::move(sGraphPending));
        sGraphPending.clear();
        LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance("General");
        if (!queue || !queue->post([lines]() { writeGraphLines(*lines); }))
        {
            writeGraphLines(*lines);
        }
    }
    void graphPart(const PieceShapes& p, bool definite, F64 t)
    {
        const U64 key = pieceKey(p) ^ (definite ? 0x5851F42D4C957F2DULL : 0) ^ (p.top_known ? std::hash<LLUUID>{}(p.top_tex) : 0);
        if (sGraphParts.count(key) || !graphOpen())
        {
            return;
        }
        sGraphParts.insert(key);
        GraphLine& l = sGraphPending.emplace_back();
        l.t = t;
        l.a = p.id;
        l.part = p.part;
        l.root = p.root;
        l.definite = definite;
        l.box = p.box;
        l.c[0] = p.cx;
        l.c[1] = p.cy;
        l.c[2] = p.zt;
        l.axis[0] = p.ux;
        l.axis[1] = p.uy;
        l.size[0] = p.lu;
        l.size[1] = p.lv;
        l.top_tex = p.top_known ? p.top_tex : LLUUID::null;
        l.nums.reserve(p.top.size() * 6);
        for (const TopEdge& e : p.top)
        {
            l.nums.insert(l.nums.end(), { F64(e.x0), F64(e.y0), F64(e.z0), F64(e.x1), F64(e.y1), F64(e.z1) });
        }
    }
    template <typename Meets> void graphMeet(const PieceShapes& a, const PieceShapes& b, const Meets& meets, F64 t)
    {
        const U64 key = pieceKey(a) * 0x9E3779B97F4A7C15ULL ^ pieceKey(b);
        if (sGraphMeets.count(key) || !graphOpen())
        {
            return;
        }
        sGraphMeets.insert(key);
        GraphLine& l = sGraphPending.emplace_back();
        l.meet = true;
        l.t = t;
        l.a = a.id;
        l.b = b.id;
        for (const auto& m : meets)
        {
            l.nums.insert(l.nums.end(), { F64(m.x), F64(m.y), F64(m.z) });
        }
    }

    // Flat parts the road runs on over, whatever their name or thickness: a run of them is road where it carries the
    // road on, meeting road at its ends at the road's height, lying along it and no wider (Pop's bridge: 2 m deck slabs
    // named "Flat;straight;no fence"). A plaza is wider, a pavement meets the road along its side, a roof or a table
    // meets none.
    constexpr F64 CARRY_END_M = 6.0;        // how near a run's end the road meeting it must lie
    constexpr F64 CARRY_MIN_M = 6.0;        // the shortest run
    constexpr F64 CARRY_ALONE_M = 30.0;     // a run met at one end only (its far end not in reach) carries on this long
    constexpr F64 CARRY_DEG = 35.0;         // how far from the road's heading it may lie
    constexpr F64 CARRY_STEP_M = 0.5;       // the tops of parts that meet: this far apart in height, and
    constexpr F64 CARRY_GRADE = 0.3;        // their slope over half the longer's length
    constexpr F64 CARRY_STRIP = 1.5;        // a run this many times longer than wide, and
    constexpr F64 CARRY_ALONE_STRIP = 2.5;  // met at one end only, this many
    constexpr F64 CARRY_PLANE_M = 0.25;     // how far a top's corners may lie off its plane (a ramp is a plane)
    bool partSquare(const PieceShapes& p);
    void partAxis(const PieceShapes& p, F64 dx, F64 dy, F64& ax, F64& ay, F64& along, F64& across);

    // Its top a plane, flat or sloping: a deck, a slab, a ramp; not a cliff's or a rock's.
    bool planarTop(const PieceShapes& p)
    {
        // least squares z = a + b x + c y about the top's middle
        F64 sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sz = 0, sxz = 0, syz = 0, n = 0;
        for (const Shape& s : p.shapes)
        {
            for (S32 k = 0; k < s.n; ++k)
            {
                const F64 x = s.x[k] - p.cx, y = s.y[k] - p.cy, z = s.zv[k];
                sxx += x * x;
                sxy += x * y;
                syy += y * y;
                sx += x;
                sy += y;
                sz += z;
                sxz += x * z;
                syz += y * z;
                n += 1;
            }
        }
        if (n < 3)
        {
            return false;
        }
        // solve [n sx sy; sx sxx sxy; sy sxy syy] [a b c] = [sz sxz syz]
        const F64 m[3][4] = { { n, sx, sy, sz }, { sx, sxx, sxy, sxz }, { sy, sxy, syy, syz } };
        auto det3 = [](F64 a00, F64 a01, F64 a02, F64 a10, F64 a11, F64 a12, F64 a20, F64 a21, F64 a22)
        {
            return a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
        };
        const F64 d = det3(m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
        if (std::fabs(d) < 1e-9)
        {
            return false;
        }
        const F64 a = det3(m[0][3], m[0][1], m[0][2], m[1][3], m[1][1], m[1][2], m[2][3], m[2][1], m[2][2]) / d;
        const F64 b = det3(m[0][0], m[0][3], m[0][2], m[1][0], m[1][3], m[1][2], m[2][0], m[2][3], m[2][2]) / d;
        const F64 c = det3(m[0][0], m[0][1], m[0][3], m[1][0], m[1][1], m[1][3], m[2][0], m[2][1], m[2][3]) / d;
        for (const Shape& s : p.shapes)
        {
            for (S32 k = 0; k < s.n; ++k)
            {
                if (std::fabs(s.zv[k] - (a + b * (s.x[k] - p.cx) + c * (s.y[k] - p.cy))) > CARRY_PLANE_M)
                {
                    return false;
                }
            }
        }
        return true;
    }

    void carryOn(F64 now)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - carryOn");
        std::vector<PieceShapes*> all;          // the doubtful parts, then the road's
        std::vector<Gathered*> owner;
        for (Gathered& g : sGathered)
        {
            for (PieceShapes* p : g.doubtful)
            {
                all.push_back(p);
                owner.push_back(&g);
            }
        }
        const S32 nd = S32(all.size());
        if (nd == 0)
        {
            return;
        }
        for (Gathered& g : sGathered)
        {
            if (g.named == 1)
            {
                for (PieceShapes* p : g.pieces)
                {
                    if (!p->top.empty())
                    {
                        all.push_back(p);
                    }
                }
            }
        }
        const S32 n = S32(all.size());
        if (n == nd)
        {
            return;
        }
        constexpr F64 PAD = 0.2;
        std::unordered_map<U64, std::vector<S32>> cells;
        auto each_cell = [](const PieceShapes& p, auto&& fn)
        {
            for (S64 gx = S64(std::floor((p.bx0 - PAD) / HASH_M)); gx <= S64(std::floor((p.bx1 + PAD) / HASH_M)); ++gx)
            {
                for (S64 gy = S64(std::floor((p.by0 - PAD) / HASH_M)); gy <= S64(std::floor((p.by1 + PAD) / HASH_M)); ++gy)
                {
                    fn(hashKey(gx, gy));
                }
            }
        };
        for (S32 i = 0; i < n; ++i)
        {
            each_cell(*all[i], [&cells, i](U64 key) { cells[key].push_back(i); });
        }
        std::vector<S32> up(nd);
        for (S32 i = 0; i < nd; ++i)
        {
            up[i] = i;
        }
        auto find = [&up](S32 i)
        {
            while (up[i] != i)
            {
                i = up[i] = up[up[i]];
            }
            return i;
        };
        struct Anchor
        {
            S32 road;
            F64 z;
        };
        std::vector<std::vector<Anchor>> anchors(nd);
        std::vector<S32> seen(n, -1);
        for (S32 i = 0; i < nd; ++i)
        {
            const PieceShapes& pi = *all[i];
            each_cell(pi, [&](U64 key)
            {
                for (S32 j : cells[key])
                {
                    if (j == i || seen[j] == i || (j < nd && j < i))
                    {
                        continue;       // a doubtful pair once, from its lower
                    }
                    seen[j] = i;
                    const PieceShapes& pj = *all[j];
                    if (pi.bx1 + PAD < pj.bx0 || pj.bx1 + PAD < pi.bx0 || pi.by1 + PAD < pj.by0 || pj.by1 + PAD < pi.by0
                        || std::fabs(pi.zt - pj.zt) > CARRY_STEP_M + 0.5 * (std::max(pi.lu, pj.lu)) * CARRY_GRADE)
                    {
                        continue;       // not at the same height (meets are worked out only for what could meet)
                    }
                    const bool i_first = pi.id < pj.id;
                    const PartPair& pp = partPair(i_first ? pi : pj, i_first ? pj : pi, now);
                    if (pp.meets.empty())
                    {
                        continue;
                    }
                    if (j < nd)
                    {
                        up[find(i)] = find(j);
                    }
                    else
                    {
                        anchors[i].push_back({ j, pp.meets.front().z });
                    }
                }
            });
        }
        std::unordered_map<S32, std::vector<S32>> runs;
        for (S32 i = 0; i < nd; ++i)
        {
            runs[find(i)].push_back(i);
        }
        std::vector<U8> carried(nd, 0);
        for (const auto& [run_id, members] : runs)
        {
            // the run's axis and extent, from its tops' corners
            F64 mx = 0, my = 0;
            S32 count = 0;
            bool any_anchor = false;
            for (S32 i : members)
            {
                any_anchor |= !anchors[i].empty();
                for (const Shape& s : all[i]->shapes)
                {
                    for (S32 k = 0; k < s.n; ++k)
                    {
                        mx += s.x[k];
                        my += s.y[k];
                        ++count;
                    }
                }
            }
            if (!any_anchor || count == 0)
            {
                continue;
            }
            mx /= count;
            my /= count;
            F64 cxx = 0, cyy = 0, cxy = 0;
            for (S32 i : members)
            {
                for (const Shape& s : all[i]->shapes)
                {
                    for (S32 k = 0; k < s.n; ++k)
                    {
                        cxx += (s.x[k] - mx) * (s.x[k] - mx);
                        cyy += (s.y[k] - my) * (s.y[k] - my);
                        cxy += (s.x[k] - mx) * (s.y[k] - my);
                    }
                }
            }
            const F64 angle = 0.5 * std::atan2(2 * cxy, cxx - cyy);
            const F64 ax = std::cos(angle), ay = std::sin(angle);
            F64 s0 = 1e30, s1 = -1e30, t0 = 1e30, t1 = -1e30;
            for (S32 i : members)
            {
                for (const Shape& s : all[i]->shapes)
                {
                    for (S32 k = 0; k < s.n; ++k)
                    {
                        const F64 sa = (s.x[k] - mx) * ax + (s.y[k] - my) * ay, ta = -(s.x[k] - mx) * ay + (s.y[k] - my) * ax;
                        s0 = std::min(s0, sa);
                        s1 = std::max(s1, sa);
                        t0 = std::min(t0, ta);
                        t1 = std::max(t1, ta);
                    }
                }
            }
            const F64 length = s1 - s0, width = t1 - t0, tmid = 0.5 * (t0 + t1);
            if (length < CARRY_MIN_M || width < MIN_BRANCH_W || length < CARRY_STRIP * width)
            {
                continue;       // a road runs on along it: a strip, not a square (a plaza, a forecourt)
            }
            if (!std::all_of(members.begin(), members.end(), [&all](S32 i) { return planarTop(*all[i]); }))
            {
                continue;       // a cliff, a rock: no deck
            }
            // the road meeting it at each end: lying past that end (not alongside), in its cross-section
            struct End
            {
                bool met = false, aligned = false, skewed = false;
                F64 z = 0;
            } ends[2];
            for (S32 i : members)
            {
                for (const Anchor& a : anchors[i])
                {
                    const PieceShapes& rp = *all[a.road];
                    const F64 sc = (rp.cx - mx) * ax + (rp.cy - my) * ay, tc = -(rp.cx - mx) * ay + (rp.cy - my) * ax;
                    if (std::fabs(tc - tmid) > 0.5 * width + 1.0)
                    {
                        continue;
                    }
                    const S32 e = sc <= s0 + CARRY_END_M ? 0 : sc >= s1 - CARRY_END_M ? 1 : -1;
                    if (e < 0)
                    {
                        continue;
                    }
                    ends[e].met = true;
                    ends[e].z = a.z;
                    if (!partSquare(rp))
                    {
                        F64 rax, ray, along, across;
                        partAxis(rp, ax, ay, rax, ray, along, across);
                        (std::fabs(rax * ax + ray * ay) >= std::cos(CARRY_DEG * PI / 180) ? ends[e].aligned : ends[e].skewed) = true;
                    }
                }
            }
            const S32 met = S32(ends[0].met) + S32(ends[1].met);
            if (met == 0 || (met == 1 && (length < CARRY_ALONE_M || length < CARRY_ALONE_STRIP * width)))
            {
                continue;
            }
            bool ok = true;
            for (S32 e = 0; e < 2 && ok; ++e)
            {
                if (!ends[e].met)
                {
                    continue;
                }
                if (ends[e].skewed && !ends[e].aligned)
                {
                    ok = false;     // the road there runs across it: a pavement, a forecourt
                    break;
                }
                // the road's width about that end, at its height: the run no wider than it by much (a plaza)
                const F64 cap = e == 0 ? s0 : s1;
                F64 r0 = 1e30, r1 = -1e30;
                for (S32 j = nd; j < n; ++j)
                {
                    const PieceShapes& rp = *all[j];
                    if (std::fabs(rp.zt - ends[e].z) > 1.0)
                    {
                        continue;
                    }
                    const F64 sc = (rp.cx - mx) * ax + (rp.cy - my) * ay, tc = -(rp.cx - mx) * ay + (rp.cy - my) * ax;
                    if (std::fabs(sc - cap) > 10.0 || std::fabs(tc - tmid) > 0.5 * width + 10.0)
                    {
                        continue;
                    }
                    for (const Shape& s : rp.shapes)
                    {
                        for (S32 k = 0; k < s.n; ++k)
                        {
                            const F64 ta = -(s.x[k] - mx) * ay + (s.y[k] - my) * ax;
                            r0 = std::min(r0, ta);
                            r1 = std::max(r1, ta);
                        }
                    }
                }
                ok = r1 > r0 && width <= 1.5 * (r1 - r0) + 2.0;
            }
            if (ok)
            {
                for (S32 i : members)
                {
                    carried[i] = 1;
                }
            }
        }
        for (S32 i = 0; i < nd; ++i)
        {
            if (carried[i])
            {
                owner[i]->carried = true;
                if (!all[i]->shapes.empty())
                {
                    owner[i]->pieces.push_back(all[i]);
                }
            }
        }
        for (Gathered& g : sGathered)
        {
            if (g.carried)
            {
                g.doubtful.erase(std::remove_if(g.doubtful.begin(), g.doubtful.end(), [&g](PieceShapes* p)
                                                { return std::find(g.pieces.begin(), g.pieces.end(), p) != g.pieces.end(); }),
                                 g.doubtful.end());
            }
        }
    }

    U64 sPartsGen = 0;      // moves whenever settleParts rebuilds the parts
    U64 sVerdictGen = 0;    // ... and whenever only their verdicts change
    void settleParts(F64 now)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - settleParts");
        // the parts it would take, and what is known of them: the same as last time, what meets what is too
        // (rebuilding its lookups every refresh cost 12 ms, two refreshes a second, standing still)
        {
            // the parts (their pieces and tops) apart from what is known of their linksets: the same parts with
            // only a verdict changed (properties arriving, a trickle for minutes after a relog) are the same meetings,
            // and only the verdicts are written again
            static U64 last_key = 0;
            U64 key = U64(sPlazaRoots.size());
            std::vector<std::pair<U8, U8>> verdicts;
            for (const Gathered& g : sGathered)
            {
                if (!roadOf(g) || sPlazaRoots.count(g.root))
                {
                    continue;
                }
                const bool definite = definiteOf(g);
                const bool pending = !definite && !host::hasProperties(g.root);
                for (const PieceShapes* pc : g.pieces)
                {
                    key = mix(key ^ pieceKey(*pc) ^ U64(reinterpret_cast<uintptr_t>(pc)) ^ (pc->top.empty() ? 8 : 0));
                    if (!pc->top.empty())
                    {
                        verdicts.push_back({ U8(definite), U8(pending) });
                    }
                }
            }
            if (key == last_key && !sParts.empty() && verdicts.size() == sParts.size())
            {
                bool moved = false;
                for (size_t i = 0; i < sParts.size(); ++i)
                {
                    sParts[i]->used = now;
                    moved |= sPartDefinite[i] != verdicts[i].first || sPartPending[i] != verdicts[i].second;
                    sPartDefinite[i] = verdicts[i].first;
                    sPartPending[i] = verdicts[i].second;
                }
                sVerdictGen += moved;
                return;
            }
            last_key = key;
        }
        ++sPartsGen;
        sParts.clear();
        sPartDefinite.clear();
        sPartPending.clear();
        sPartIndex.clear();
        sWalkParts.clear();
        // A narrow top is road only where it carries on a wide road top at its height: a rod (rebar out of a broken
        // deck, a pole, a cylinder at any tilt: its top narrower than ROD_W_M across in plan) never is, and a rail, a
        // railing or a kerb wall (narrower than LONE_W_M) is not unless a top at least LONE_W_M wide lies within
        // NEAR_M of it in plan and within FLUSH_M of its height.
        constexpr F64 ROD_W_M = 0.5, LONE_W_M = 2.5, NEAR_M = 1.0, FLUSH_M = 0.3;
        std::vector<const PieceShapes*> tops;
        for (const Gathered& g : sGathered)
        {
            if (roadOf(g) && !sPlazaRoots.count(g.root))
            {
                for (const PieceShapes* p : g.pieces)
                {
                    if (!p->top.empty())
                    {
                        tops.push_back(p);
                    }
                }
            }
        }
        auto rail = [&tops](const PieceShapes* p)
        {
            if (p->lv >= LONE_W_M)
            {
                return false;
            }
            bool flush = false;
            for (const PieceShapes* q : tops)
            {
                if (q != p && q->lv >= LONE_W_M && std::fabs(p->zt - q->zt) <= FLUSH_M
                    && p->bx0 <= q->bx1 + NEAR_M && q->bx0 <= p->bx1 + NEAR_M
                    && p->by0 <= q->by1 + NEAR_M && q->by0 <= p->by1 + NEAR_M)
                {
                    flush = true;
                    break;
                }
            }
            if (p->lv < ROD_W_M || !flush)
            {
                LL_DEBUGS("SKRoad") << "part " << p->part << " of " << p->root << " (" << p->lv << " m wide, "
                                    << p->lu << " m long, top " << p->zt << ") is a " << (p->lv < ROD_W_M ? "rod" : "rail")
                                    << ", not road" << LL_ENDL;
                return true;
            }
            return false;
        };
        for (const Gathered& g : sGathered)
        {
            if (!roadOf(g) || sPlazaRoots.count(g.root))
            {
                continue;
            }
            const bool definite = definiteOf(g);
            const bool pending = !definite && !host::hasProperties(g.root);
            for (PieceShapes* p : g.pieces)
            {
                if (!p->top.empty() && !rail(p))
                {
                    sPartIndex[p] = S32(sParts.size());
                    sParts.push_back(p);
                    sPartDefinite.push_back(definite);
                    sPartPending.push_back(pending);
                }
            }
        }
        const S32 n = S32(sParts.size());
        static std::unordered_map<U64, std::vector<S32>> cells;
        for (auto& cell : cells)
        {
            cell.second.clear();
        }
        if (cells.size() > 8192)
        {
            cells.clear();
        }
        constexpr F64 PAD = 0.2;
        auto each_cell = [](const PieceShapes& p, auto&& fn)
        {
            for (S64 gx = S64(std::floor((p.bx0 - PAD) / HASH_M)); gx <= S64(std::floor((p.bx1 + PAD) / HASH_M)); ++gx)
            {
                for (S64 gy = S64(std::floor((p.by0 - PAD) / HASH_M)); gy <= S64(std::floor((p.by1 + PAD) / HASH_M)); ++gy)
                {
                    fn(hashKey(gx, gy));
                }
            }
        };
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - settleParts cells");
            for (S32 i = 0; i < n; ++i)
            {
                each_cell(*sParts[i], [i](U64 key) { cells[key].push_back(i); });
            }
        }
        static Timer since_evict;
        if (since_evict.getElapsedTimeF64() > 30.0 || sPartPairs.size() > 4 * CACHE_MAX_PAIRS)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - evict part pairs");
            since_evict.reset();
            for (auto it = sPartPairs.begin(); it != sPartPairs.end();)
            {
                it = now - it->second.used > CACHE_KEEP_S ? sPartPairs.erase(it) : std::next(it);
            }
            // past four times the cap the oldest go, down to the cap: nothing is old enough for CACHE_KEEP_S, so the
            // test above alone kept the map over the cap and walked its millions of pairs on every call (120-190 ms,
            // twice a second, after a few hours' driving)
            if (sPartPairs.size() > 4 * CACHE_MAX_PAIRS)
            {
                std::vector<std::pair<F64, U64>> old;
                old.reserve(sPartPairs.size());
                for (const auto& [key, pair] : sPartPairs)
                {
                    old.push_back({ pair.used, key });
                }
                const size_t drop = old.size() - CACHE_MAX_PAIRS;
                std::nth_element(old.begin(), old.begin() + drop, old.end());
                for (size_t i = 0; i < drop; ++i)
                {
                    sPartPairs.erase(old[i].second);
                }
            }
        }
        const F64 graph_t = LLTimer::getTotalSeconds().value();
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - graphPart");
            for (S32 i = 0; i < n; ++i)
            {
                graphPart(*sParts[i], sPartDefinite[i] != 0, graph_t);
            }
        }
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - settleParts meets");
            sConts.assign(n, {});
            std::vector<S32> seen(n, -1);
            for (S32 i = 0; i < n; ++i)
            {
                const PieceShapes& pi = *sParts[i];
                each_cell(pi, [&](U64 key)
                {
                    for (S32 j : cells[key])
                    {
                        if (j <= i || seen[j] == i)
                        {
                            continue;
                        }
                        seen[j] = i;
                        const PieceShapes& pj = *sParts[j];
                        if (pi.bx1 + PAD < pj.bx0 || pj.bx1 + PAD < pi.bx0 || pi.by1 + PAD < pj.by0 || pj.by1 + PAD < pi.by0)
                        {
                            continue;
                        }
                        const bool i_first = pi.id < pj.id;
                        const PartPair& pp = partPair(i_first ? pi : pj, i_first ? pj : pi, now);
                        const S32 lo = i_first ? i : j, hi = i_first ? j : i;
                        for (const Meet& m : pp.meets)
                        {
                            sConts[lo].push_back({ hi, m.edge_a, m.edge_b, m.x, m.y, m.z, m.ax, m.ay, m.bx, m.by });
                            sConts[hi].push_back({ lo, m.edge_b, m.edge_a, m.x, m.y, m.z, m.ax, m.ay, m.bx, m.by });
                        }
                        if (!pp.meets.empty())
                        {
                            graphMeet(*sParts[lo], *sParts[hi], pp.meets, graph_t);
                        }
                    }
                });
            }
        }
        flushGraph();
    }

    // The pairs of roads in reach (indices in sActiveRoots, the lower first) whose parts meet (sConts).
    void partStates(std::vector<PartState>& out)
    {
        out.clear();
        for (const Gathered& g : sGathered)
        {
            const bool definite = definiteOf(g);
            const bool pending = !definite && g.named != 0 && !host::hasProperties(g.root);
            const bool plaza = sPlazaRoots.count(g.root) != 0;
            auto add = [&](const PieceShapes* p, bool doubtful)
            {
                PartState& s = out.emplace_back();
                s.part = p->part;
                s.root = g.root;
                s.named = S8(g.named);
                s.definite = definite;
                s.pending = pending;
                s.carried = g.carried && !doubtful;
                s.doubtful = doubtful;
                s.plaza = plaza;
                s.walkable = sPartIndex.count(p) != 0;
            };
            for (const PieceShapes* p : g.pieces)
            {
                add(p, false);
            }
            for (const PieceShapes* p : g.doubtful)
            {
                add(p, true);
            }
        }
    }

    void rootPairsFromParts(std::vector<U64>& out)
    {
        const S32 n = S32(sActiveRoots.size());
        std::unordered_map<LLUUID, S32> index;
        index.reserve(n);
        for (S32 a = 0; a < n; ++a)
        {
            index[sActiveRoots[a]->root] = a;
        }
        std::unordered_set<U64> pairs;
        for (S32 i = 0; i < S32(sParts.size()) && i < S32(sConts.size()); ++i)
        {
            auto ia = index.find(sParts[i]->root);
            if (ia == index.end())
            {
                continue;
            }
            for (const Continuation& c : sConts[i])
            {
                auto ib = index.find(sParts[c.other]->root);
                if (ib == index.end() || ib->second == ia->second)
                {
                    continue;
                }
                const S32 a = std::min(ia->second, ib->second), b = std::max(ia->second, ib->second);
                pairs.insert((U64(U32(a)) << 32) | U32(b));
            }
        }
        out.assign(pairs.begin(), pairs.end());
    }

    bool partSquare(const PieceShapes& p)
    {
        return std::max(p.lu, p.lv) < SQUARE_RATIO * std::min(p.lu, p.lv);
    }

    // The part's axis nearest a direction, facing it: how long it is that way and how wide across.
    void partAxis(const PieceShapes& p, F64 dx, F64 dy, F64& ax, F64& ay, F64& along, F64& across)
    {
        const F64 du = p.ux * dx + p.uy * dy, dv = -p.uy * dx + p.ux * dy;
        if (std::fabs(du) >= std::fabs(dv))
        {
            ax = du >= 0 ? p.ux : -p.ux;
            ay = du >= 0 ? p.uy : -p.uy;
            along = p.lu;
            across = p.lv;
        }
        else
        {
            ax = dv >= 0 ? -p.uy : p.uy;
            ay = dv >= 0 ? p.ux : -p.ux;
            along = p.lv;
            across = p.lu;
        }
    }

    // The definite part under the car, or the nearest along the way it faces within ROUTE_NEAR_M; a part whose
    // linkset's properties have not come, under the car, where none is.
    S32 partUnder(F64 x, F64 y, F64 z, F64 hx, F64 hy)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - partUnder");
        S32 pending_under = -1;
        auto it = sHash.find(hashKey(S64(std::floor(x / HASH_M)), S64(std::floor(y / HASH_M))));
        if (it != sHash.end())
        {
            for (S32 i : it->second)
            {
                const Shape& sh = *sShapes[i];
                if (!sh.piece || std::fabs(heightAt(sh, x, y) - z) >= 2.5 || !inside(sh, x, y))
                {
                    continue;
                }
                auto f = sPartIndex.find(sh.piece);
                if (f == sPartIndex.end())
                {
                    continue;
                }
                F64 ax, ay, along, across;
                partAxis(*sh.piece, hx, hy, ax, ay, along, across);
                if (across < MIN_BRANCH_W)
                {
                    continue;   // a centre-line strip or a catseye laid on the road
                }
                if (sPartDefinite[f->second])
                {
                    return f->second;
                }
                if (sPartPending[f->second] && pending_under < 0)
                {
                    pending_under = f->second;
                }
            }
        }
        S32 best = -1;
        F64 best_d = ROUTE_NEAR_M;
        const bool route_fresh = !sRoute.empty() && sClock.getElapsedTimeF64() - sRouteTime < ROUTE_KEEP_S;
        for (S32 k = 0; k < S32(sParts.size()); ++k)
        {
            const PieceShapes& p = *sParts[k];
            if (!sPartDefinite[k] || std::min(p.lu, p.lv) < MIN_BRANCH_W || std::fabs(p.zt - z) > Z_REACH
                || (!partSquare(p) && std::fabs(p.ux * hx + p.uy * hy) < 0.5))
            {
                continue;
            }
            const F64 du = std::fabs((x - p.cx) * p.ux + (y - p.cy) * p.uy) - p.lu / 2;
            const F64 dv = std::fabs(-(x - p.cx) * p.uy + (y - p.cy) * p.ux) - p.lv / 2;
            F64 d = std::hypot(std::max(0.0, du), std::max(0.0, dv));
            if (route_fresh && sRoute.count(p.root))
            {
                d -= 1.0;       // the road it was on
            }
            if (d < best_d)
            {
                best_d = d;
                best = k;
            }
        }
        if (best < 0 && pending_under >= 0)
        {
            if (sAskFirstSet.insert(sParts[pending_under]->root).second)
            {
                sAskFirst.push_back(sParts[pending_under]->root);
            }
            return pending_under;
        }
        return best;
    }

    // The part's top's height at a point (its mean where no face of it holds the point).
    F64 partZ(const PieceShapes& p, F64 x, F64 y)
    {
        for (const Shape& sh : p.shapes)
        {
            if (x >= sh.x0 && x <= sh.x1 && y >= sh.y0 && y <= sh.y1 && inside(sh, x, y))
            {
                return heightAt(sh, x, y);
            }
        }
        return p.zt;
    }

    // section() on the given parts' own faces alone, whatever their surface.
    bool sectionOnParts(F64 x, F64 y, F64 z, F64 hx, F64 hy, S32 a, S32 b, F64& left, F64& right, F64& zroad)
    {
        const F64 rx = hy, ry = -hx;
        const F64 lx0 = std::min(x - rx * REACH_M, x + rx * REACH_M) - 1e-6, lx1 = std::max(x - rx * REACH_M, x + rx * REACH_M) + 1e-6;
        const F64 ly0 = std::min(y - ry * REACH_M, y + ry * REACH_M) - 1e-6, ly1 = std::max(y - ry * REACH_M, y + ry * REACH_M) + 1e-6;
        sSpans.clear();
        sSpanZ.clear();
        for (S32 slot = 0; slot < (a == b ? 1 : 2); ++slot)
        {
            const S32 k = slot == 0 ? a : b;
            for (const Shape& sh : sParts[k]->shapes)
            {
                if (sh.x1 < lx0 || sh.x0 > lx1 || sh.y1 < ly0 || sh.y0 > ly1 || std::fabs(sh.z - z) > Z_REACH)
                {
                    continue;
                }
                addSpan(sh, x, y, rx, ry);
            }
        }
        return mergeSpans(left, right, zroad);
    }

    // The lane along a line of points on the road's parts: each measured across the parts it lies on (its
    // middle the road's, its length the width), two lanes on a part wide enough for two (the right one's
    // middle, keep offset as ever), the part itself where it is a lane's width; every PATH_STEP from the car on.
    // Bring the road's edges across a point (offsets to the right of the way (ux, uy), left < 0 < right) in to
    // where the air shows a wall or rail within AIR_EDGE_CLEAR_M, walking out from the point every 0.25 m.
    // Where the air has no floor there yet, the edge stays as measured.
    constexpr F64 AIR_EDGE_CLEAR_M = 0.3;
    void clipToAir(F64 x, F64 y, F64 z, F64 ux, F64 uy, F64& left, F64& right)
    {
        const F64 rx = uy, ry = -ux;
        F64 floor, clear;
        if (!airFloor(x, y, z, MAX_STEP_UP_M, Z_REACH, floor, clear))
        {
            return;     // no air here: nothing to go on
        }
        for (int side : { -1, 1 })
        {
            F64& edge = side < 0 ? left : right;
            const F64 limit = std::fabs(edge);
            F64 zr = floor;
            for (F64 d = 0.25; d <= limit; d += 0.25)
            {
                F64 f, c;
                const F64 px = x + rx * d * side, py = y + ry * d * side;
                if (!airFloor(px, py, zr, MAX_STEP_UP_M, 1.0, f, c) || c < AIR_EDGE_CLEAR_M)
                {
                    edge = side * std::max(0.5, d - 0.25);     // the last clear step
                    break;
                }
                zr = f;
            }
        }
    }

    constexpr F64 SEAM_NUDGE_M = 0.1;           // a meeting point is measured this far on into the next part
    constexpr F64 SIDE_ROAD_DEG = 45.0;         // a branch turning more than this off the way is a side road

    struct PartPoint
    {
        F64 x, y;
        S32 a, b;       // the parts it lies on (one, or the two it is between)
    };
    constexpr F64 LAYBY_LEN_M = 30.0, LAYBY_DEG = 25.0, LAYBY_CLEAR_M = 0.5, LAYBY_LINE_M = 3.0, LAYBY_GAP_M = 3.0;
    // The road's middle and width are continuous: a section measured short for a few points (a seam, a gap
    // between pieces, a slab on the road) takes its neighbours' width, and the middle moves aside no faster than
    // LANE_SLIDE across per metre along, so one bad section bends the line into a gentle shift, never a kink.
    constexpr F64 LANE_WIDTH_BLIP_M = 0.75, LANE_SLIDE = 0.15, LANE_SLIDE_FREE_M = 0.05, LANE_KINK_REACH_M = 8.0, LANE_KINK_M = 0.4;
    const F64 LANE_SLIDE_STRAIGHT_COS = std::cos(10.0 * PI / 180);
    void keepLaneContinuous(std::vector<LanePoint>& out, const std::vector<PartPoint>& base, F64 keep)
    {
        const size_t n = out.size();
        if (n < 3)
        {
            return;
        }
        std::vector<F64> cx(n), cy(n), w(n);
        for (size_t i = 0; i < n; ++i)
        {
            cx[i] = (out[i].left_x + out[i].right_x) / 2;
            cy[i] = (out[i].left_y + out[i].right_y) / 2;
            w[i] = out[i].road_w;
        }
        std::vector<F64> fw = w;
        for (size_t i = 0; i < n; ++i)
        {
            F64 win[5];
            S32 k = 0;
            for (size_t j = i >= 2 ? i - 2 : 0; j <= std::min(n - 1, i + 2); ++j)
            {
                win[k++] = w[j];
            }
            std::sort(win, win + k);
            const F64 med = win[k / 2];
            if (std::fabs(w[i] - med) > LANE_WIDTH_BLIP_M)
            {
                // the short measure lost one edge: keep the edge that agrees with the neighbours' middle
                const F64 nx = out[i].ay, ny = -out[i].ax;      // to the right
                const F64 pm = i > 0 ? (cx[i - 1] - cx[i]) * nx + (cy[i - 1] - cy[i]) * ny : 0.0;
                const F64 qm = i + 1 < n ? (cx[i + 1] - cx[i]) * nx + (cy[i + 1] - cy[i]) * ny : pm;
                const F64 nb = (i > 0 && i + 1 < n) ? (pm + qm) / 2 : pm;
                const F64 lo = -w[i] / 2, hi = w[i] / 2;        // its edges, across from its middle
                const F64 from_left = lo + med / 2, from_right = hi - med / 2;
                const F64 shift = std::fabs(from_left - nb) < std::fabs(from_right - nb) ? from_left : from_right;
                cx[i] += nx * shift;
                cy[i] += ny * shift;
                fw[i] = med;
                ++sView.lane_kinks;
            }
        }
        // the lane point itself (a lane count changing moves it by a lane's shift) slides no faster than LANE_SLIDE
        std::vector<F64> px(n), py(n);
        std::vector<S32> lanes(n);
        for (size_t i = 0; i < n; ++i)
        {
            lanes[i] = fw[i] >= TWO_LANES_M ? 2 : 1;
            const F64 shift = lanes[i] == 2 ? laneShiftIn(fw[i], keep) : 0.0;
            px[i] = cx[i] + out[i].ay * shift;
            py[i] = cy[i] - out[i].ax * shift;
        }
        // a kink that comes back: on a straight run, a point off the chord of the line LANE_KINK_REACH_M before and
        // after it by more than LANE_KINK_M is a blip, and is laid back on the chord; a lasting shift is no blip
        std::vector<F64> s_along(n, 0.0);
        for (size_t i = 1; i < n; ++i)
        {
            s_along[i] = s_along[i - 1] + std::hypot(base[i].x - base[i - 1].x, base[i].y - base[i - 1].y);
        }
        std::vector<char> blip(n, 0);
        for (size_t i = 1; i + 1 < n; ++i)
        {
            size_t j = i, k = i;
            while (j > 0 && s_along[i] - s_along[j] < LANE_KINK_REACH_M) --j;
            while (k + 1 < n && s_along[k] - s_along[i] < LANE_KINK_REACH_M) ++k;
            if (s_along[i] - s_along[j] < LANE_KINK_REACH_M / 2 || s_along[k] - s_along[i] < LANE_KINK_REACH_M / 2
                || out[j].ax * out[k].ax + out[j].ay * out[k].ay < LANE_SLIDE_STRAIGHT_COS)
            {
                continue;
            }
            const F64 cxk = px[k] - px[j], cyk = py[k] - py[j], L = std::hypot(cxk, cyk);
            if (L < 1.0)
            {
                continue;
            }
            const F64 dev = ((px[i] - px[j]) * cyk - (py[i] - py[j]) * cxk) / L;
            blip[i] = std::fabs(dev) > LANE_KINK_M;
        }
        for (size_t i = 1; i + 1 < n; ++i)
        {
            if (!blip[i])
            {
                continue;
            }
            size_t j = i - 1, k = i + 1;
            while (j > 0 && blip[j]) --j;
            while (k + 1 < n && blip[k]) ++k;
            const F64 t = (s_along[i] - s_along[j]) / std::max(1e-6, s_along[k] - s_along[j]);
            px[i] = px[j] + (px[k] - px[j]) * t;
            py[i] = py[j] + (py[k] - py[j]) * t;
            ++sView.lane_kinks;
        }
        for (size_t i = 0; i < n; ++i)
        {
            LanePoint& lp = out[i];
            const F64 ux = lp.ax, uy = lp.ay;
            lp.x = px[i];
            lp.y = py[i];
            lp.lane_w = fw[i] / lanes[i];
            lp.road_w = fw[i];
            lp.lanes = lanes[i];
            lp.left_x = cx[i] - uy * fw[i] / 2;
            lp.left_y = cy[i] + ux * fw[i] / 2;
            lp.right_x = cx[i] + uy * fw[i] / 2;
            lp.right_y = cy[i] - ux * fw[i] / 2;
        }
    }

    // A line that hooks aside in its last EDGE_HOOK_M at a region's edge has met the edge before the next region's
    // pieces came: whatever it turned onto there (the road's other half, a kerb) is not where the road goes. The
    // hook goes and the line ends where it left its heading: run on straight past a road's end cap at an edge it
    // drove onto the grass (route5-end), and the short line already slows the car and holds it for the edge. A
    // hook onto the road's kerb strip ends its lane point a lane's width or more inside the edge (6.4 m at
    // Dammastock's west edge), so the line's end need only lie within EDGE_HOOK_NEAR_M of it.
    constexpr F64 REGION_M = 256.0, EDGE_HOOK_M = 15.0, EDGE_HOOK_NEAR_M = 8.0;
    const F64 EDGE_HOOK_COS = std::cos(45.0 * PI / 180);
    F64 toRegionEdge(F64 x, F64 y)
    {
        const F64 fx = x - std::floor(x / REGION_M) * REGION_M, fy = y - std::floor(y / REGION_M) * REGION_M;
        return std::min({ fx, REGION_M - fx, fy, REGION_M - fy });
    }
    void straightenEdgeHook(std::vector<LanePoint>& out)
    {
        if (out.size() < 4 || toRegionEdge(out.back().x, out.back().y) > EDGE_HOOK_NEAR_M)
        {
            return;
        }
        size_t i = out.size() - 1;
        F64 back = 0;
        while (i > 0 && back < EDGE_HOOK_M)
        {
            back += std::hypot(out[i].x - out[i - 1].x, out[i].y - out[i - 1].y);
            --i;
        }
        if (i == 0)
        {
            return;
        }
        F64 hx = out[i].x - out[i - 1].x, hy = out[i].y - out[i - 1].y;
        const F64 hl = std::hypot(hx, hy);
        const size_t e = out.size() - 1;
        F64 ex = out[e].x - out[e - 1].x, ey = out[e].y - out[e - 1].y;
        const F64 el = std::hypot(ex, ey);
        if (hl < 1e-6 || el < 1e-6 || (hx * ex + hy * ey) / (hl * el) > EDGE_HOOK_COS)
        {
            return;
        }
        hx /= hl;
        hy /= hl;
        // the hook's start: the first point past i that leaves the heading by more than a lane's slide
        size_t k = i;
        while (k + 1 < out.size()
               && std::fabs((out[k + 1].x - out[i].x) * hy - (out[k + 1].y - out[i].y) * hx) < 0.5)
        {
            ++k;
        }
        out.resize(k + 1);
        ++sView.edge_hooks;
    }

    // The lane at each point of a walked line: across the whole road there, kept continuous.
    std::vector<LanePoint> partLaneRaw(const std::vector<PartPoint>& line, F64 dx, F64 dy, F64 z, F64 keep, bool hook = true)
    {
        std::vector<LanePoint> out;
        // The whole road across, not the part walked: a road of lane-wide parts beside full-width ones read 5 m
        // on one and 10 m on the next, and the lane flipped between a part's middle and a quarter off the
        // road's (a zigzag). Every road linkset's faces in reach count (definite or pending, not a strip).
        F64 last_raw_w = -1, last_raw_mx = 0, last_raw_my = 0;  // the last measure as taken, kept or not
        std::unordered_set<LLUUID> road_roots;
        for (const Segment& g : sSegments)
        {
            if ((g.definite || g.pending) && g.width >= MIN_BRANCH_W)
            {
                road_roots.insert(g.root);
            }
        }
        // a side road the walk turned away from at a junction is not this road's width: its mouth moved the centre
        // line toward it (unless the walk is on that linkset too)
        for (const Junction& jn : sView.junctions)
        {
            for (S32 k = 0; k < S32(jn.branches.size()); ++k)
            {
                const Branch& br = jn.branches[k];
                if (k != jn.chosen && std::fabs(br.turn_deg) > SIDE_ROAD_DEG
                    && std::find(sChainRoots.begin(), sChainRoots.end(), br.root) == sChainRoots.end())
                {
                    road_roots.erase(br.root);
                }
            }
        }
        // a lay-by or a bus stop beside the carriageway is beside the road, not of its width (Nangrim's "R9 Layby"
        // read the road 16.5 m wide and pulled the lane into it): a linkset off the walk, no longer than LAYBY_LEN_M,
        // lying along the road wholly beside the walked line, and carried on by no road linkset in line with it
        for (const Segment& g : sSegments)
        {
            if (!road_roots.count(g.root) || g.hi - g.lo > LAYBY_LEN_M || g.square
                || std::find(sChainRoots.begin(), sChainRoots.end(), g.root) != sChainRoots.end())
            {
                continue;
            }
            size_t at = 0;
            F64 best = 1e18;
            for (size_t i = 0; i < line.size(); ++i)
            {
                const F64 d = std::hypot(line[i].x - g.cx, line[i].y - g.cy);
                if (d < best)
                {
                    best = d;
                    at = i;
                }
            }
            if (line.empty())
            {
                break;
            }
            const size_t a = at > 0 ? at - 1 : at, b = at + 1 < line.size() ? at + 1 : at;
            const F64 lx = line[b].x - line[a].x, ly = line[b].y - line[a].y, L = std::hypot(lx, ly);
            if (L < 1e-6 || std::fabs((g.ax * lx + g.ay * ly) / L) < std::cos(LAYBY_DEG * PI / 180))
            {
                continue;
            }
            const F64 aside = std::fabs((g.cx - line[at].x) * ly - (g.cy - line[at].y) * lx) / L;
            if (aside < g.width / 2 + LAYBY_CLEAR_M)
            {
                continue;       // under the walked line, not beside it
            }
            bool carried = false;
            for (const Segment& o : sSegments)
            {
                if (&o == &g || o.root == g.root || !(o.definite || o.pending))
                {
                    continue;
                }
                const F64 along = (o.cx - g.cx) * g.ax + (o.cy - g.cy) * g.ay;
                const F64 off = std::fabs(-(o.cx - g.cx) * g.ay + (o.cy - g.cy) * g.ax);
                const F64 gap = std::fabs(along) - (g.hi - g.lo) / 2 - (o.hi - o.lo) / 2;
                carried |= off < LAYBY_LINE_M && gap < LAYBY_GAP_M && std::fabs(o.ax * g.ax + o.ay * g.ay) > std::cos(LAYBY_DEG * PI / 180);
            }
            if (!carried)
            {
                road_roots.erase(g.root);
            }
        }
        for (size_t i = 0; i < line.size(); ++i)
        {
            const size_t a = i > 0 ? i - 1 : i, b = i + 1 < line.size() ? i + 1 : i;
            F64 ux = line[b].x - line[a].x, uy = line[b].y - line[a].y;
            const F64 L = std::hypot(ux, uy);
            if (L < 1e-6)
            {
                ux = dx;
                uy = dy;
            }
            else
            {
                ux /= L;
                uy /= L;
            }
            const PieceShapes& pa = *sParts[line[i].a];
            F64 cx = line[i].x, cy = line[i].y;
            F64 ax, ay, along, w;
            partAxis(pa, ux, uy, ax, ay, along, w);
            F64 left, right, zr;
            bool across = false;
            // a point where two parts meet is measured just inside the next: across the seam itself, the line ran
            // along the edges of the pieces laid beside them in the same grid and missed them (the road read 2.5 m
            // of 11 at every other point); along the way, so its offsets across are the point's
            const F64 nudge = line[i].a != line[i].b ? SEAM_NUDGE_M : 0.0;
            const F64 sx = line[i].x + ux * nudge, sy = line[i].y + uy * nudge, sz = partZ(*sParts[line[i].b], sx, sy);
            {
                sAllowedRoots = road_roots;
                sAnySurface = true;
                sWayOnly = true;
                sWayX = ux;
                sWayY = uy;
                sWayKeep[0] = &pa;
                sWayKeep[1] = sParts[line[i].b];
                across = section(sx, sy, sz, ux, uy, left, right, zr) && right - left > 0.5
                         && left <= 0.5 && right >= -0.5;     // the run the point is on
                sWayOnly = false;
                sAnySurface = false;
                sAllowedRoots.clear();
            }
            if (across || (sectionOnParts(sx, sy, sz, ux, uy, line[i].a, line[i].b, left, right, zr)
                           && right - left > 0.5))
            {
                // the faces run on past a bridge's guardrails (walkways, the deck's edges): each edge is walked in
                // from the point and stops short of a wall or rail, as the air's clearance tells it
                clipToAir(sx, sy, sz, ux, uy, left, right);
                F64 mid = (left + right) / 2;
                F64 wm = right - left;
                // one bad measure must not kink the line: a width jumping past 1.5 times the last, or a middle
                // moving 2 m aside from it, keeps the last point's width and offset
                const F64 raw_w = wm;
                const F64 raw_mx = line[i].x + uy * mid, raw_my = line[i].y - ux * mid;
                if (!out.empty())
                {
                    const LanePoint& prev = out.back();
                    const F64 pmx = (prev.left_x + prev.right_x) / 2, pmy = (prev.left_y + prev.right_y) / 2;
                    const F64 prev_off = (pmx - line[i].x) * uy - (pmy - line[i].y) * ux;     // its middle, across here
                    const F64 raw_off = (last_raw_mx - line[i].x) * uy - (last_raw_my - line[i].y) * ux;
                    auto unlike = [&](F64 w0, F64 off0) { return wm > 1.5 * w0 || wm < w0 / 1.5 || std::fabs(mid - off0) > 2.0; };
                    // a real change (a road widening) agrees with the last raw measure too: two in a row stand
                    if (unlike(prev.road_w, prev_off) && (last_raw_w <= 0 || unlike(last_raw_w, raw_off)))
                    {
                        mid = prev_off;
                        wm = prev.road_w;
                        ++sView.width_fallbacks;
                    }
                }
                last_raw_w = raw_w;
                last_raw_mx = raw_mx;
                last_raw_my = raw_my;
                cx = line[i].x + uy * mid;      // section's offsets are to the right: (uy, -ux)
                cy = line[i].y - ux * mid;
                w = wm;
            }
            else
            {
                ++sView.width_fallbacks;
            }
            const S32 lanes = w >= TWO_LANES_M ? 2 : 1;
            const F64 shift = lanes == 2 ? laneShiftIn(w, keep) : 0.0;
            LanePoint lp;
            lp.x = cx + uy * shift;
            lp.y = cy - ux * shift;
            lp.ax = ux;
            lp.ay = uy;
            lp.lane_w = w / lanes;
            lp.road_w = w;
            lp.lanes = lanes;
            lp.z = z;
            lp.left_x = cx - uy * w / 2;
            lp.left_y = cy + ux * w / 2;
            lp.right_x = cx + uy * w / 2;
            lp.right_y = cy - ux * w / 2;
            out.push_back(lp);
        }
        keepLaneContinuous(out, line, keep);
        if (hook)
        {
            straightenEdgeHook(out);
        }
        return out;
    }

    // Lane points every PATH_STEP between the laid ones (and, given each laid one's place along the road, theirs).
    std::vector<LanePoint> densify(const std::vector<LanePoint>& out, const std::vector<F64>* s_in = nullptr, std::vector<F64>* s_out = nullptr)
    {
        std::vector<LanePoint> dense;
        for (size_t i = 0; i + 1 < out.size(); ++i)
        {
            const LanePoint& a = out[i];
            const LanePoint& b = out[i + 1];
            const F64 L = std::hypot(b.x - a.x, b.y - a.y);
            const S32 n = std::max(1, S32(std::ceil(L / PATH_STEP)));
            for (S32 k = 0; k < n; ++k)
            {
                const F64 t = F64(k) / n;
                LanePoint q = a;
                q.x = a.x + (b.x - a.x) * t;
                q.y = a.y + (b.y - a.y) * t;
                q.left_x = a.left_x + (b.left_x - a.left_x) * t;
                q.left_y = a.left_y + (b.left_y - a.left_y) * t;
                q.right_x = a.right_x + (b.right_x - a.right_x) * t;
                q.right_y = a.right_y + (b.right_y - a.right_y) * t;
                if (L > 1e-6)
                {
                    q.ax = (b.x - a.x) / L;
                    q.ay = (b.y - a.y) / L;
                }
                dense.push_back(q);
                if (s_in && s_out)
                {
                    s_out->push_back((*s_in)[i] + ((*s_in)[i + 1] - (*s_in)[i]) * t);
                }
            }
        }
        if (!out.empty())
        {
            dense.push_back(out.back());
            if (s_in && s_out)
            {
                s_out->push_back(s_in->back());
            }
        }
        return dense;
    }

    // The first of a line's points the car (x, y) has not passed.
    size_t firstAhead(const std::vector<LanePoint>& dense, F64 x, F64 y, size_t from = 0)
    {
        size_t first = from;
        while (first + 1 < dense.size() && (dense[first + 1].x - x) * dense[first + 1].ax + (dense[first + 1].y - y) * dense[first + 1].ay < 0)
        {
            ++first;
        }
        return first;
    }

    std::vector<LanePoint> partLane(const std::vector<PartPoint>& line, F64 x, F64 y, F64 z, F64 dx, F64 dy, F64 keep)
    {
        std::vector<LanePoint> dense = densify(partLaneRaw(line, dx, dy, z, keep));
        // the controller wants the line from the car on: drop what lies behind it
        dense.erase(dense.begin(), dense.begin() + firstAhead(dense, x, y));
        return dense;
    }

    // The line's length ahead of the car, metres.
    F64 lineSeen()
    {
        F64 seen = 0;
        for (size_t i = 0; i + 1 < sPath.size(); ++i)
        {
            seen += std::hypot(sPath[i + 1].x - sPath[i].x, sPath[i + 1].y - sPath[i].y);
        }
        return seen;
    }
    constexpr F64 SURFACE_TURN_DEG = 10.0, SURFACE_SHIFT_M = 0.5;
    F64 sLineDirX = 1, sLineDirY = 0, sLineDirAt = -1e9, sNow = 0;

    // Degrees between a line's way where it passes the car and the car's heading (hx, hy).
    F64 lineHeadingOff(const std::vector<LanePoint>& line, const LLVector3d& pos, F64 hx, F64 hy)
    {
        size_t at = 0;
        F64 best = 1e18;
        for (size_t i = 0; i < line.size(); ++i)
        {
            const F64 d = (line[i].x - pos.mdV[VX]) * (line[i].x - pos.mdV[VX]) + (line[i].y - pos.mdV[VY]) * (line[i].y - pos.mdV[VY]);
            if (d < best)
            {
                best = d;
                at = i;
            }
        }
        const size_t b = std::min(line.size() - 1, at + 3), a = b > 3 ? b - 3 : 0;
        const F64 lx = line[b].x - line[a].x, ly = line[b].y - line[a].y, L = std::hypot(lx, ly);
        if (L < 0.5)
        {
            return 0.0;
        }
        return std::acos(std::clamp((lx * hx + ly * hy) / L, -1.0, 1.0)) * 180 / PI;
    }

    // ---------------------------------------------------------------- the line holding its lane

    constexpr F64 LANE_JUMP_M = 2.5, LANE_JUMP_S = 1.5;
    F64 sLaneJumpSince = -1.0;

    // The car's signed distance (m, + right) from a line's nearest stretch.
    F64 sideOf(const std::vector<LanePoint>& line, const LLVector3d& pos)
    {
        F64 best = 1e18, side = 0;
        for (size_t i = 0; i + 1 < line.size(); ++i)
        {
            const LanePoint& a = line[i];
            const LanePoint& b = line[i + 1];
            const F64 sx = b.x - a.x, sy = b.y - a.y, L2 = std::max(1e-9, sx * sx + sy * sy);
            const F64 f = std::clamp(((pos.mdV[VX] - a.x) * sx + (pos.mdV[VY] - a.y) * sy) / L2, 0.0, 1.0);
            const F64 dx = pos.mdV[VX] - (a.x + f * sx), dy = pos.mdV[VY] - (a.y + f * sy);
            const F64 d2 = dx * dx + dy * dy;
            if (d2 < best)
            {
                best = d2;
                side = (sx * dy - sy * dx) > 0 ? -std::sqrt(d2) : std::sqrt(d2);
            }
        }
        return side;
    }

    // The road's end: past the last piece of the driving surface lies only ground (Linden ground plates, unnamed
    // and so taken for road by name, grass, rocks, a picnic table), not a bridge or a junction of another
    // surface that leads back onto the road. A meeting from the surface onto a part not of it is a way on only
    // where the parts beyond it, within END_LOOK_M ahead, reach the surface again (Route 5's end at Xanthorhoe:
    // the walk went on from the rounded cap over the ground plates, and the line into a picnic table).
    constexpr F64 END_LOOK_M = 40.0;
    // pieceMatches without its cache (which keeps an answer given before the part's textures came)
    bool ofSurface(const PieceShapes& p)
    {
        return sDriveTex.empty() || !p.textures_known
               || std::any_of(p.textures.begin(), p.textures.end(), [](const LLUUID& t) { return sDriveTex.count(t) > 0; });
    }
    bool leadsToSurface(S32 from, S32 start, F64 x, F64 y, F64 dx, F64 dy)
    {
        static std::vector<U32> seen;
        static U32 gen = 0;
        ++gen;
        if (seen.size() < sParts.size())
        {
            seen.resize(sParts.size(), 0);
        }
        seen[from] = gen;
        seen[start] = gen;
        std::vector<S32> todo{ start };
        while (!todo.empty())
        {
            const S32 k = todo.back();
            todo.pop_back();
            for (const Continuation& c : sConts[k])
            {
                if (seen[c.other] == gen || (!sPartDefinite[c.other] && !sPartPending[c.other]))
                {
                    continue;
                }
                seen[c.other] = gen;
                const F64 ahead = (c.x - x) * dx + (c.y - y) * dy;
                if (ahead < -1.0 || std::hypot(c.x - x, c.y - y) > END_LOOK_M)
                {
                    continue;
                }
                if (ofSurface(*sParts[c.other]))
                {
                    return true;
                }
                todo.push_back(c.other);
            }
        }
        return false;
    }

    // A jump: where the walk runs out at a part's far edge (a ramp's lip, a broken bridge), the road going on beyond
    // along the same way, across a gap no wider than JUMP_MAX_M, at about the lip's height or below it (Fujin's
    // ramp over the water, 32 m from lip to landing, the broken span sagging into the water between; it was taken
    // for a dead end and the car turned round). The landing is the first part of the same kind (its top's texture,
    // or both of the driving surface) under the way on, lined up with it.
    constexpr F64 JUMP_MIN_M = 2.0, JUMP_MAX_M = 40.0, JUMP_STEP_M = 0.5, JUMP_RUNUP_M = 8.0, JUMP_RAMP_M = 1.0, JUMP_GAP_M = 4.0;
    constexpr F64 JUMP_RISE_M = 1.0, JUMP_DROP_M = 4.0, JUMP_ALIGN_COS = 0.8;
    bool partHeightAt(const PieceShapes& p, F64 x, F64 y, F64& h)
    {
        bool found = false;
        for (const Shape& sh : p.shapes)
        {
            if (x >= sh.tx0 && x <= sh.tx1 && y >= sh.ty0 && y <= sh.ty1 && inside(sh, x, y))
            {
                const F64 hs = heightAt(sh, x, y);
                if (!found || hs > h)
                {
                    h = hs;
                    found = true;
                }
            }
        }
        return found;
    }
    S32 jumpLanding(S32 from, const std::vector<U32>& visited, U32 walk, F64 x, F64 y, F64 z, F64 dx, F64 dy,
                    F64& lx, F64& ly, F64& lz)
    {
        const PieceShapes& lip = *sParts[from];
        for (F64 d = JUMP_MIN_M; d <= JUMP_MAX_M; d += JUMP_STEP_M)
        {
            const F64 qx = x + d * dx, qy = y + d * dy;
            auto it = sHash.find(hashKey(S64(std::floor(qx / HASH_M)), S64(std::floor(qy / HASH_M))));
            if (it == sHash.end())
            {
                continue;
            }
            for (S32 i : it->second)
            {
                const Shape& sh = *sShapes[i];
                if (!sh.piece || !inside(sh, qx, qy))
                {
                    continue;
                }
                const F64 h = heightAt(sh, qx, qy);
                if (h > z + JUMP_RISE_M || h < z - JUMP_DROP_M)
                {
                    continue;
                }
                auto f = sPartIndex.find(sh.piece);
                if (f == sPartIndex.end() || f->second == from || (f->second < S32(visited.size()) && visited[f->second] == walk)
                    || !sPartDefinite[f->second])
                {
                    continue;
                }
                const PieceShapes& pn = *sParts[f->second];
                F64 ax, ay, along, across;
                partAxis(pn, dx, dy, ax, ay, along, across);
                const bool same = (lip.top_known && pn.top_known) ? lip.top_tex == pn.top_tex : (ofSurface(lip) && ofSurface(pn));
                if (!same || across < MIN_BRANCH_W || (!partSquare(pn) && ax * dx + ay * dy < JUMP_ALIGN_COS))
                {
                    continue;
                }
                lx = qx;
                ly = qy;
                lz = h;
                return f->second;
            }
        }
        return -1;
    }

    // How far on from (px, py) along (dx, dy) the part's top ends: a box's edge the way crosses (the farthest
    // corner's projection overshot it on a heading across the box), else its outline's farthest point that way
    // (a mesh's outline is its faces' union, not one edge to cross).
    F64 farSide(const PieceShapes& pc, F64 px, F64 py, F64 dx, F64 dy)
    {
        F64 through = 0;
        F64 exit = 1e18;
        for (const TopEdge& e : pc.top)
        {
            through = std::max(through, std::max((e.x0 - px) * dx + (e.y0 - py) * dy, (e.x1 - px) * dx + (e.y1 - py) * dy));
            const F64 ex = e.x1 - e.x0, ey = e.y1 - e.y0;
            const F64 det = dx * ey - dy * ex;
            if (std::fabs(det) < 1e-9 || dx * e.ox + dy * e.oy <= 0)
            {
                continue;
            }
            const F64 vx = e.x0 - px, vy = e.y0 - py;
            const F64 t = (vx * ey - vy * ex) / det;
            const F64 u = (vx * dy - vy * dx) / det;
            if (t >= 0 && u >= 0 && u <= 1)
            {
                exit = std::min(exit, t);
            }
        }
        return pc.box && exit < 1e18 ? exit : through;
    }

    // The committed route: the parts driven over and the way chosen ahead, in order, as a driver keeps it. Each walk
    // follows it from where the car is and only extends it at its far end; the parts behind the car retire into its
    // history (the way travelled, which "straight" at a junction is measured against). A choice once made stands
    // until the part it chose stops being a way on (not road, gone, no longer met); then the route is repaired from
    // there on, the part driven kept. It is seeded afresh only when the car has truly left it (off it for a while,
    // a teleport, turned round) or on a new run (forgetRoad), and View::route says so.
    struct RouteStep
    {
        const PieceShapes* part = nullptr;
        LLUUID id;                      // its part (the pointer checked against it: a cache entry can be reused)
        F64 x = 0, y = 0;               // where the route enters it (the seed: the car's point on it)
        bool jump = false;              // entered across a gap (a ramp's lip): left to the walk to find again
        bool has_junction = false;      // entered by a choice at a junction (read again each walk, see follow)
        F64 dx = 0, dy = 0;             // the way it was entered (where a walk resumes from it)
    };
    std::deque<RouteStep> sCommitted;
    S32 sRepairDepth = 0;                       // inside a walk afresh (below)
    F64 sLastTravelled = 0;                     // how far the last walk went
    constexpr F64 ROUTE_FRESH_M = 10.0;         // a walk afresh this much further than the route's repairs it
    constexpr F64 ROUTE_FRESH_KEEP_M = 60.0;    // ... and keeps the route's way where the route reached this far
    F64 sOffRouteSince = -1, sRouteWalkAt = -1e9, sTurnedSince = -1;
    constexpr F64 ROUTE_TURNED_DEG = 50.0;      // the car's heading this far off the route's way ...
    constexpr F64 ROUTE_TURNED_S = 0.5;         // ... for this long: turned off it
    F64 sRouteCarX = 0, sRouteCarY = 0;
    constexpr F64 ROUTE_OFF_M = 6.0;            // the car this far from the route's line is off it (more than a lane)
    constexpr F64 ROUTE_OFF_S = 1.0;            // ... for this long, and the route is seeded afresh
    constexpr F64 ROUTE_TELEPORT_M = 40.0;      // the car this far from where it was at the last walk: a teleport
    constexpr F64 ROUTE_HISTORY_M = 60.0;       // how much of the way travelled is kept behind the car
    constexpr F64 ROUTE_HEADING_M = 12.0;       // the incoming heading at a junction: over this much of the way travelled
    constexpr F64 INCOMING_DEG = 30.0;          // ... measured from where the part was entered this far off it (from its side)

    StoredRoad sStored;
    bool sPathFromStore = false;
    std::vector<LanePoint> sRoadPath;

    const std::vector<LanePoint>& roadForView()
    {
        return sPathFromStore && !sRoadPath.empty() ? sRoadPath : sPath;
    }

    void forgetRoute()
    {
        sCommitted.clear();
        sStored.clear();
        sOffRouteSince = -1;
        sTurnedSince = -1;
        sRouteWalkAt = -1e9;
    }

    S32 routeIndex(const RouteStep& s)
    {
        auto f = sPartIndex.find(s.part);
        return f != sPartIndex.end() && s.part->part == s.id ? f->second : -1;
    }

    // ---------------------------------------------------------------- the stored road (StoredRoad)

    constexpr F64 STORE_MARGIN_M = 3.0;         // a change this near a stored section's footprint re-lays it
    constexpr F64 STORE_END_M = 20.0;           // ... and this near the far end of a walk that stopped there walks on

    // What one walk found, for the stored road: its line from where it started, the route steps it went through and
    // what it met on them, each by the walked step it was met on.
    struct WalkOut
    {
        std::vector<PartPoint> line;
        size_t base = 1;                        // line[base - 1] is where it started: the car's point, or a step's entry
        std::vector<RouteStep> walked;
        std::vector<size_t> walked_line;        // each walked step's entry in line (where it started, for the first)
        size_t prefilled = 1;                   // walked steps the stored road had already
        std::vector<std::pair<size_t, Junction>> junctions;
        std::vector<std::pair<size_t, View::SurfaceChange>> surface_changes;
        std::vector<std::pair<size_t, StoredRoad::SignHit>> signs;
        size_t jump_step = 0;                   // the walked step a gap was crossed from (View::jump)
        bool ended = false, stuck = false;
        F64 dx = 1, dy = 0;                     // the walk's last way
    };
    WalkOut sWalkOut;

    S32 storeStep(const LLUUID& part)
    {
        for (S32 k = S32(sStored.steps.size()); k-- > 0;)
        {
            if (sStored.steps[k].part == part)
            {
                return k;
            }
        }
        return -1;
    }

    // The stored step a place along the road lies on.
    S32 storeStepAt(F64 s)
    {
        const std::vector<StoredRoad::Step>& st = sStored.steps;
        auto it = std::upper_bound(st.begin(), st.end(), s, [](F64 v, const StoredRoad::Step& k) { return v < k.s; });
        return it == st.begin() ? 0 : S32(it - st.begin()) - 1;
    }

    // Where the car is along the stored road: its nearest stretch, near where it was at the last refresh.
    void storeLocate(F64 x, F64 y)
    {
        StoredRoad& r = sStored;
        const size_t from = r.car_i > 40 ? r.car_i - 40 : 0;
        F64 best = 1e18;
        for (size_t i = from; i + 1 < r.dense.size() && r.dense_s[i] < r.car_s + 2 * ROUTE_TELEPORT_M; ++i)
        {
            const LanePoint &a = r.dense[i], &b = r.dense[i + 1];
            const F64 ex = b.x - a.x, ey = b.y - a.y, L2 = ex * ex + ey * ey;
            const F64 t = L2 > 1e-9 ? std::clamp(((x - a.x) * ex + (y - a.y) * ey) / L2, 0.0, 1.0) : 0.0;
            const F64 d = std::hypot(x - a.x - t * ex, y - a.y - t * ey);
            if (d < best)
            {
                best = d;
                r.car_i = i;
                r.car_s = r.dense_s[i] + t * (r.dense_s[i + 1] - r.dense_s[i]);
            }
        }
        if (r.dense.size() == 1)
        {
            r.car_i = 0;
            r.car_s = r.dense_s[0];
        }
    }

    // Everything from step k on is walked again: what lies past cut_s goes, and what was met on step k or after.
    void storeTruncate(size_t k, F64 cut_s)
    {
        StoredRoad& r = sStored;
        if (k >= r.steps.size())
        {
            return;
        }
        const U32 serial = r.steps[k].serial;
        r.steps.resize(k + 1);
        while (!r.line.empty() && r.line.back().s > cut_s + 1e-6)
        {
            r.line.pop_back();
            r.lanes.pop_back();
        }
        while (!r.dense.empty() && r.dense_s.back() > cut_s + 1e-6)
        {
            r.dense.pop_back();
            r.dense_raw.pop_back();
            r.dense_s.pop_back();
        }
        auto gone = [serial](const auto& e) { return e.first >= serial; };
        r.junctions.erase(std::remove_if(r.junctions.begin(), r.junctions.end(), gone), r.junctions.end());
        r.surface_changes.erase(std::remove_if(r.surface_changes.begin(), r.surface_changes.end(), gone), r.surface_changes.end());
        r.signs.erase(std::remove_if(r.signs.begin(), r.signs.end(), [serial](const StoredRoad::SignHit& h) { return h.step >= serial; }),
                      r.signs.end());
        r.jump &= r.jump_step < serial;
        r.ended = r.road_end = r.stuck = false;
        r.walk_end.clear();
        r.car_i = std::min(r.car_i, r.dense.empty() ? 0 : r.dense.size() - 1);
    }

    // What lies further behind the car than the route's history goes.
    void storeDropBehind()
    {
        StoredRoad& r = sStored;
        const F64 from_s = r.car_s - ROUTE_HISTORY_M;
        size_t k = 0;
        while (k + 1 < r.steps.size() && r.steps[k + 1].s <= from_s)
        {
            ++k;
        }
        if (k > 0)
        {
            const U32 serial = r.steps[k].serial;
            r.steps.erase(r.steps.begin(), r.steps.begin() + k);
            auto old = [serial](const auto& e) { return e.first < serial; };
            r.junctions.erase(std::remove_if(r.junctions.begin(), r.junctions.end(), old), r.junctions.end());
            r.surface_changes.erase(std::remove_if(r.surface_changes.begin(), r.surface_changes.end(), old), r.surface_changes.end());
            r.signs.erase(std::remove_if(r.signs.begin(), r.signs.end(), [serial](const StoredRoad::SignHit& h) { return h.step < serial; }),
                          r.signs.end());
            r.jump &= r.jump_step >= serial;
            size_t n = 0;
            while (n < r.line.size() && r.line[n].step < serial)
            {
                ++n;
            }
            r.line.erase(r.line.begin(), r.line.begin() + n);
            r.lanes.erase(r.lanes.begin(), r.lanes.begin() + n);
        }
        size_t n = 0;
        while (n + 1 < r.dense.size() && r.dense_s[n + 1] < from_s)
        {
            ++n;
        }
        if (n > 0)
        {
            r.dense.erase(r.dense.begin(), r.dense.begin() + n);
            r.dense_raw.erase(r.dense_raw.begin(), r.dense_raw.begin() + n);
            r.dense_s.erase(r.dense_s.begin(), r.dense_s.begin() + n);
            r.car_i = r.car_i > n ? r.car_i - n : 0;
        }
    }

    // The cells the stored road's sections cover (with STORE_MARGIN_M round each), each with its sections, and the
    // area round a stopped walk's far end.
    struct StoreCorridor
    {
        std::unordered_map<U64, std::vector<U32>> cells;
        F64 ex0 = 1, ey0 = 1, ex1 = 0, ey1 = 0;     // the far end's area, where the walk stopped there
        bool overlaps(U32 i, F64 bx0, F64 by0, F64 bx1, F64 by1) const
        {
            const LanePoint& p = sStored.dense[i];
            return std::min({ p.x, p.left_x, p.right_x }) - STORE_MARGIN_M <= bx1 && std::max({ p.x, p.left_x, p.right_x }) + STORE_MARGIN_M >= bx0
                   && std::min({ p.y, p.left_y, p.right_y }) - STORE_MARGIN_M <= by1 && std::max({ p.y, p.left_y, p.right_y }) + STORE_MARGIN_M >= by0;
        }
        // the first section a box touches (dense index), or the far end's (the last), or -1
        S64 first(F64 bx0, F64 by0, F64 bx1, F64 by1, U32 floor = 0) const
        {
            S64 best = -1;
            for (S64 gx = S64(std::floor(bx0 / HASH_M)); gx <= S64(std::floor(bx1 / HASH_M)); ++gx)
            {
                for (S64 gy = S64(std::floor(by0 / HASH_M)); gy <= S64(std::floor(by1 / HASH_M)); ++gy)
                {
                    auto f = cells.find(hashKey(gx, gy));
                    if (f == cells.end())
                    {
                        continue;
                    }
                    for (U32 i : f->second)
                    {
                        if (i >= floor && (best < 0 || S64(i) < best) && overlaps(i, bx0, by0, bx1, by1))
                        {
                            best = i;
                        }
                    }
                }
            }
            if (best < 0 && ex0 <= ex1 && bx0 <= ex1 && bx1 >= ex0 && by0 <= ey1 && by1 >= ey0)
            {
                best = S64(sStored.dense.size()) - 1;
            }
            return best;
        }
    };
    void storeCorridor(StoreCorridor& c)
    {
        const StoredRoad& r = sStored;
        for (U32 i = 0; i < U32(r.dense.size()); ++i)
        {
            const LanePoint& p = r.dense[i];
            const F64 x0 = std::min({ p.x, p.left_x, p.right_x }) - STORE_MARGIN_M, x1 = std::max({ p.x, p.left_x, p.right_x }) + STORE_MARGIN_M;
            const F64 y0 = std::min({ p.y, p.left_y, p.right_y }) - STORE_MARGIN_M, y1 = std::max({ p.y, p.left_y, p.right_y }) + STORE_MARGIN_M;
            for (S64 gx = S64(std::floor(x0 / HASH_M)); gx <= S64(std::floor(x1 / HASH_M)); ++gx)
            {
                for (S64 gy = S64(std::floor(y0 / HASH_M)); gy <= S64(std::floor(y1 / HASH_M)); ++gy)
                {
                    std::vector<U32>& v = c.cells[hashKey(gx, gy)];
                    if (v.empty() || v.back() != i)
                    {
                        v.push_back(i);
                    }
                }
            }
        }
        if (r.ended && !r.dense.empty())
        {
            const LanePoint& e = r.dense.back();
            c.ex0 = e.x - STORE_END_M;
            c.ex1 = e.x + STORE_END_M;
            c.ey0 = e.y - STORE_END_M;
            c.ey1 = e.y + STORE_END_M;
        }
    }

    U64 partState(const PieceShapes& p, U8 definite, U8 pending)
    {
        return mix(pieceKey(p) ^ (U64(definite) << 1) ^ (U64(pending) << 2) ^ (p.textures_known ? 8 : 0)
                   ^ (p.top_known ? U64(std::hash<LLUUID>()(p.top_tex)) << 4 : 0));
    }
    U64 signsKey()
    {
        U64 key = sSignCandidates.size();
        for (const SignCandidate& s : sSignCandidates)
        {
            key = mix(key ^ U64(std::hash<LLUUID>()(s.root)) ^ U64(S64(s.x * 10)) ^ (U64(S64(s.y * 10)) << 20));
        }
        return key;
    }

    // What the stored road was laid over, written again (the road parts, air tiles and sign candidates in reach
    // of it), and the first stored section touched by what changed since: its dense index, -1 where nothing
    // did. Why goes to `why`.
    S64 storeSeen(bool compare, std::string& why)
    {
        StoredRoad& r = sStored;
        const U32 floor = U32(r.car_i);     // what changed behind the car is the way travelled: it stands
        StoreCorridor c;
        storeCorridor(c);
        S64 first = -1;
        auto touch = [&first](S64 i)
        {
            if (i >= 0 && (first < 0 || i < first))
            {
                first = i;
            }
        };
        if (!compare || r.parts_gen != sPartsGen || r.verdict_gen != sVerdictGen)
        {
            S32 added = 0, gone = 0, changed = 0;
            LLUUID first_part;
            S64 first_at = -1;
            std::unordered_map<LLUUID, StoredRoad::PartSeen> seen;
            for (size_t i = 0; i < sParts.size(); ++i)
            {
                const PieceShapes& p = *sParts[i];
                const S64 at = c.first(p.bx0, p.by0, p.bx1, p.by1);
                if (at < 0)
                {
                    continue;
                }
                const StoredRoad::PartSeen now{ partState(p, sPartDefinite[i], sPartPending[i]), p.bx0, p.by0, p.bx1, p.by1 };
                seen[p.part] = now;
                if (!compare)
                {
                    continue;
                }
                auto f = r.parts_seen.find(p.part);
                if (f == r.parts_seen.end() || f->second.state != now.state)
                {
                    ++(f == r.parts_seen.end() ? added : changed);
                    const S64 ahead = c.first(p.bx0, p.by0, p.bx1, p.by1, floor);
                    if (ahead >= 0 && (first_at < 0 || ahead < first_at))
                    {
                        first_at = ahead;
                        first_part = p.part;
                    }
                }
            }
            if (compare)
            {
                for (const auto& [id, was] : r.parts_seen)
                {
                    if (!seen.count(id))
                    {
                        const S64 at = c.first(was.bx0, was.by0, was.bx1, was.by1, floor);
                        if (at >= 0)
                        {
                            ++gone;
                            if (first_at < 0 || at < first_at)
                            {
                                first_at = at;
                                first_part = id;
                            }
                        }
                    }
                }
                if (added + gone + changed > 0)
                {
                    touch(first_at);
                    why += llformat("parts added %d, gone %d, changed %d (first %s%s) ", added, gone, changed,
                                    first_part.asString().substr(0, 8).c_str(), first_at < 0 ? ", all behind the car" : "");
                }
            }
            r.parts_seen.swap(seen);
            r.parts_gen = sPartsGen;
            r.verdict_gen = sVerdictGen;
        }
        auto diffRects = [&](std::vector<std::pair<U64, std::array<F64, 4>>>& was, std::vector<std::pair<U64, std::array<F64, 4>>>& now,
                             const char* what)
        {
            std::sort(now.begin(), now.end());
            if (compare)
            {
                S32 n = 0;
                S64 at_min = -1;
                auto check = [&](const std::array<F64, 4>& b)
                {
                    const S64 at = c.first(b[0], b[1], b[2], b[3], floor);
                    if (at >= 0)
                    {
                        ++n;
                        at_min = at_min < 0 ? at : std::min(at_min, at);
                    }
                };
                std::vector<std::pair<U64, std::array<F64, 4>>> diff;
                std::set_symmetric_difference(was.begin(), was.end(), now.begin(), now.end(), std::back_inserter(diff));
                for (const auto& d : diff)
                {
                    check(d.second);
                }
                if (at_min >= 0)
                {
                    touch(at_min);
                    why += llformat("%s %d ", what, n);
                }
            }
            was.swap(now);
        };
        if (!compare || r.air_rev != sAirRevision)
        {
            std::vector<std::pair<U64, std::array<F64, 4>>> now;
            for (const AirTile& a : sAirTiles)
            {
                if (c.first(a.x0, a.y0, a.x1, a.y1) >= 0)
                {
                    now.push_back({ U64(reinterpret_cast<uintptr_t>(a.air.get())), { a.x0, a.y0, a.x1, a.y1 } });
                }
            }
            diffRects(r.air_seen, now, "air tiles");
            r.air_rev = sAirRevision;
        }
        const U64 sign_key = signsKey();
        if (!compare || r.sign_key != sign_key)
        {
            std::vector<std::pair<U64, std::array<F64, 4>>> now;
            for (const SignCandidate& s : sSignCandidates)
            {
                const std::array<F64, 4> b{ s.x - END_SIGN_BESIDE_M, s.y - END_SIGN_BESIDE_M, s.x + END_SIGN_BESIDE_M, s.y + END_SIGN_BESIDE_M };
                if (c.first(b[0], b[1], b[2], b[3]) >= 0)
                {
                    now.push_back({ U64(std::hash<LLUUID>()(s.root)) ^ U64(S64(s.x * 10)) ^ (U64(S64(s.y * 10)) << 20), b });
                }
            }
            diffRects(r.signs_seen, now, "signs");
            r.sign_key = sign_key;
        }
        if (compare && r.tex_gen != sDriveTexGen)
        {
            touch(floor);
            why += "driving surface changed ";
        }
        r.tex_gen = sDriveTexGen;
        return first;
    }

    size_t sStoreOutFirst = 0;      // the stored point the last path handed out (storeOut) starts at

    // The stored road ahead of the car laid out for this refresh: the view's junctions, surface changes, signs, jump
    // and end as the stored road has them from the car's step on, its roots, and the lane line from the car on.
    // The stored road is one road, laid one way: its line never folds back on itself and its left edge stays on its
    // left. A store that does (a turn round or a re-lay leaving the road walked the other way in it: Arpeggio, the
    // overlay's amber line split in two near the car and the edges crossed the road) holds two roads at once; where
    // and why, or false.
    bool storeFolds(const std::vector<LanePoint>& d, std::string& why)
    {
        F64 px = 0, py = 0;
        bool have = false;
        for (size_t i = 0; i + 1 < d.size(); ++i)
        {
            const F64 dx = d[i + 1].x - d[i].x, dy = d[i + 1].y - d[i].y, l = std::hypot(dx, dy);
            if (l < 0.5 || l > 8.0)
            {
                continue;       // a repeat or a jump
            }
            const F64 ux = dx / l, uy = dy / l;
            if (have && ux * px + uy * py < -0.5)
            {
                why = llformat("line folds back at %zu of %zu (%.1f, %.1f)", i, d.size(), d[i].x, d[i].y);
                return true;
            }
            px = ux;
            py = uy;
            have = true;
            // the left edge to the left of the way laid: cross(way, left - centre) > 0
            const LanePoint& q = d[i];
            if (q.road_w < OPEN_W && ux * (q.left_y - q.y) - uy * (q.left_x - q.x) < -0.3
                && ux * (q.right_y - q.y) - uy * (q.right_x - q.x) > 0.3)
            {
                why = llformat("edges laid the other way at %zu of %zu (%.1f, %.1f)", i, d.size(), q.x, q.y);
                return true;
            }
        }
        return false;
    }
    bool sStoreFolded = false;      // storeOut found the store holding two roads: dropped at the next walk

    std::vector<LanePoint> storeOut(F64 x, F64 y, S32 at, F64 length)
    {
        StoredRoad& r = sStored;
        const S32 k_at = at >= 0 ? storeStep(sCommitted[at].id) : 0;
        const U32 car_serial = r.steps.empty() ? 0 : r.steps[std::max(k_at, 0)].serial;
        // what was met from the car's step on, as far as a walk from the car would have gone (its length): the road
        // walked further at a higher speed is kept, but reported as the look-ahead reaches it
        U32 reach_serial = car_serial;
        for (const StoredRoad::Step& st : r.steps)
        {
            if (st.serial >= car_serial && st.s - r.car_s < length)
            {
                reach_serial = std::max(reach_serial, st.serial);
            }
        }
        auto in_reach = [&](U32 step) { return step >= car_serial && step <= reach_serial; };
        sView.junctions.clear();
        for (const auto& [step, j] : r.junctions)
        {
            if (in_reach(step))
            {
                sView.junctions.push_back(j);
            }
        }
        sView.surface_changes.clear();
        for (const auto& [step, sc] : r.surface_changes)
        {
            if (in_reach(step))
            {
                sView.surface_changes.push_back(sc);
            }
        }
        sView.sign_speed_limit = 0.f;
        sView.sign_warning = 0;
        for (const StoredRoad::SignHit& h : r.signs)
        {
            if (!in_reach(h.step) || h.s < r.car_s + 1.0)
            {
                continue;
            }
            if (h.what == S32(SignClass::SpeedLimit))
            {
                if (sView.sign_speed_limit <= 0.f || h.s - r.car_s < sView.sign_speed_at)
                {
                    sView.sign_speed_limit = h.value;
                    sView.sign_speed_at = h.s - r.car_s;
                }
            }
            else if (sView.sign_warning == 0 || h.s - r.car_s < sView.sign_warning_at)
            {
                sView.sign_warning = h.what;
                sView.sign_warning_at = h.s - r.car_s;
            }
        }
        sView.jump = r.jump && in_reach(r.jump_step);
        if (sView.jump)
        {
            sView.jump_x = r.jump_x;
            sView.jump_y = r.jump_y;
            sView.jump_z = r.jump_z;
            sView.land_x = r.land_x;
            sView.land_y = r.land_y;
            sView.land_z = r.land_z;
            sView.jump_dx = r.jump_dx;
            sView.jump_dy = r.jump_dy;
        }
        sView.road_end = r.road_end && !r.steps.empty() && reach_serial == r.steps.back().serial;
        if (sView.road_end)
        {
            sView.road_end_x = r.road_end_x;
            sView.road_end_y = r.road_end_y;
            sView.road_end_dx = r.road_end_dx;
            sView.road_end_dy = r.road_end_dy;
        }
        sView.walk_end = r.walk_end;
        // the road's linksets, behind the car and ahead; the parts from the car's step on
        sChainRoots.clear();
        for (const StoredRoad::Step& st : r.steps)
        {
            if (sChainRoots.empty() || sChainRoots.back() != st.root)
            {
                sChainRoots.push_back(st.root);
            }
        }
        sWalkParts.clear();
        for (S32 i = std::max(at, 0); i < S32(sCommitted.size()); ++i)
        {
            const S32 k = routeIndex(sCommitted[i]);
            if (k >= 0)
            {
                sWalkParts.push_back(k);
            }
            if (!r.steps.empty() && sCommitted[i].id == r.steps.back().part)
            {
                break;
            }
        }
        sRoadPath = r.dense;
        std::string fold_why;
        // said when its reason changes: a fold that persists is found again on every walk
        static std::string fold_said;
        if (storeFolds(r.dense, fold_why))
        {
            // shown as it is this once (the overlay draws the model), dropped and walked afresh from the car next
            if (fold_why != fold_said)
            {
                LL_INFOS("SKRoadDriver") << "road store: holds two roads (" << fold_why << "): dropped" << LL_ENDL;
                fold_said = fold_why;
            }
            sStoreFolded = true;
        }
        else
        {
            fold_said.clear();
        }
        const size_t first = firstAhead(r.dense, x, y, r.car_i > 2 ? r.car_i - 2 : 0);
        sStoreOutFirst = std::min(first, r.dense.size());
        std::vector<LanePoint> out(r.dense.begin() + std::min(first, r.dense.size()), r.dense.end());
        if (!r.lanes.empty() && !out.empty())
        {
            // the line's end hooking aside at a region's edge, run on straight as each walk's line was: tested on the
            // laid points from the car on, the path cut where the hook starts (the stored road keeps it, and walks
            // on past it once the next region's pieces come)
            size_t i0 = 0;
            while (i0 + 1 < r.line.size() && r.line[i0 + 1].s <= r.car_s)
            {
                ++i0;
            }
            std::vector<LanePoint> laid(r.lanes.begin() + i0, r.lanes.end());
            const size_t n = laid.size();
            straightenEdgeHook(laid);
            if (laid.size() < n && !laid.empty())
            {
                const F64 cut = r.line[i0 + laid.size() - 1].s;
                size_t keep_n = 0;
                while (keep_n < out.size() && r.dense_s[sStoreOutFirst + keep_n] <= cut + 1e-6)
                {
                    ++keep_n;
                }
                const size_t dropped = out.size() - std::max<size_t>(keep_n, 1);
                out.resize(out.size() - dropped);
                sRoadPath.resize(sRoadPath.size() - dropped);
            }
        }
        return out;
    }

    // [1 2 1] twice along the line (its points and the road's edges), each point from fixed on but the last.
    void smoothLine(std::vector<LanePoint>& p, size_t fixed)
    {
        if (p.size() < 3)
        {
            return;
        }
        for (S32 pass = 0; pass < 2; ++pass)
        {
            std::vector<std::array<F64, 6>> xy(p.size());
            for (size_t i = 0; i < p.size(); ++i)
            {
                xy[i] = { p[i].x, p[i].y, p[i].left_x, p[i].left_y, p[i].right_x, p[i].right_y };
            }
            for (size_t i = std::max<size_t>(1, fixed); i + 1 < p.size(); ++i)
            {
                F64 v[6];
                for (S32 k = 0; k < 6; ++k)
                {
                    v[k] = (xy[i - 1][k] + 2 * xy[i][k] + xy[i + 1][k]) / 4;
                }
                p[i].x = v[0];
                p[i].y = v[1];
                p[i].left_x = v[2];
                p[i].left_y = v[3];
                p[i].right_x = v[4];
                p[i].right_y = v[5];
            }
        }
    }

    // One walk's line and what it met, laid onto the stored road: from the car (the stored road kept up to the car)
    // or from the entry of the step it resumed at; walked[0] is the car's step, the stored road's step k0.
    void storeLay(const WalkOut& w, size_t k0, F64 s0, bool from_car, F64 cx, F64 cy, F64 cz, F64 keep)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - store lay");
        StoredRoad& r = sStored;
        if (w.line.empty() || w.walked.empty())
        {
            return;
        }
        std::vector<U32> serial(w.walked.size(), 0);
        for (size_t i = 0; i < w.prefilled && k0 + i < r.steps.size(); ++i)
        {
            serial[i] = r.steps[k0 + i].serial;
        }
        if (r.steps.empty())
        {
            // a stored road begun here: its first step is the car's
            serial[0] = r.next_serial++;
            r.steps.push_back({ w.walked[0].id, w.walked[0].part ? w.walked[0].part->root : LLUUID::null, s0, serial[0] });
        }
        // a hook at a region's edge is run on straight where the path is handed out (storeOut), not here: it is the
        // line's end as it is now, and the stored road walks on past it once the next region's pieces come
        const std::vector<LanePoint> lanes = partLaneRaw(w.line, w.dx, w.dy, cz, keep, false);
        const size_t first_new = from_car ? 0 : w.base;
        const size_t lanes_before = r.lanes.size();
        const size_t base_eff = std::min(w.base, lanes.size());
        F64 s = s0;
        size_t wi = 0;
        for (size_t j = first_new; j < w.line.size() && j < lanes.size(); ++j)
        {
            if (j > 0 && !(from_car && j == first_new))
            {
                s += std::hypot(w.line[j].x - w.line[j - 1].x, w.line[j].y - w.line[j - 1].y);
            }
            while (wi + 1 < w.walked.size() && w.walked_line[wi + 1] <= j)
            {
                ++wi;
            }
            if (serial[wi] == 0)
            {
                serial[wi] = r.next_serial++;
                r.steps.push_back({ w.walked[wi].id, w.walked[wi].part ? w.walked[wi].part->root : LLUUID::null, s, serial[wi] });
            }
            const PartPoint& pp = w.line[j];
            const LLUUID a = pp.a >= 0 && pp.a < S32(sParts.size()) ? sParts[pp.a]->part : LLUUID::null;
            const LLUUID b = pp.b >= 0 && pp.b < S32(sParts.size()) ? sParts[pp.b]->part : LLUUID::null;
            r.line.push_back({ pp.x, pp.y, s, a, b, serial[wi] });
            r.lanes.push_back(lanes[j]);
        }
        // the lane every PATH_STEP from where the stored road ends, each point's place along it the walked line's
        // (the steps' places are, and the stored road is cut by them)
        // a resumed walk's last stored lane points are laid again with their new neighbours (the one at the step's
        // entry faced the old way on: where the road turns there, a hook)
        size_t redo = 0;
        if (!from_car && lanes_before > 0)
        {
            if (lanes_before > 1 && base_eff > 1)
            {
                redo = std::min<size_t>({ 2, lanes_before - 1, base_eff - 1 });
                for (size_t k = 0; k < redo; ++k)
                {
                    r.lanes[lanes_before - redo + k] = lanes[base_eff - redo + k];
                }
            }
            const F64 cut = r.line[lanes_before - redo - 1].s;
            while (!r.dense.empty() && r.dense_s.back() > cut + 1e-6)
            {
                r.dense.pop_back();
                r.dense_raw.pop_back();
                r.dense_s.pop_back();
            }
        }
        std::vector<LanePoint> chunk;
        std::vector<F64> chunk_s, dense_s;
        const size_t from_lane = lanes_before - redo;
        const bool joined = !from_car && from_lane > 0;
        if (joined)
        {
            chunk.push_back(r.lanes[from_lane - 1]);
            chunk_s.push_back(r.line[from_lane - 1].s);
        }
        chunk.insert(chunk.end(), r.lanes.begin() + from_lane, r.lanes.end());
        for (size_t j = from_lane; j < r.line.size(); ++j)
        {
            chunk_s.push_back(r.line[j].s);
        }
        std::vector<LanePoint> dense = densify(chunk, &chunk_s, &dense_s);
        if (joined && !dense.empty())
        {
            dense.erase(dense.begin());
            dense_s.erase(dense_s.begin());
        }
        if (dense.empty())
        {
            return;
        }
        if (!r.dense_s.empty())
        {
            // after what is kept of the stored road (cut at a step's entry, or at the car)
            size_t n = 0;
            while (n < dense.size() && dense_s[n] <= r.dense_s.back() + 1e-6)
            {
                ++n;
            }
            dense.erase(dense.begin(), dense.begin() + n);
            dense_s.erase(dense_s.begin(), dense_s.begin() + n);
        }
        // on the road's surface beneath it, its height carried on from the stored road's (or the car's)
        F64 zref = (from_car || r.dense.empty()) ? surfaceAt(cx, cy, cz - 0.5) : r.dense.back().z;
        for (LanePoint& lp : dense)
        {
            lp.z = zref = surfaceAt(lp.x, lp.y, zref);
        }
        // smoothed as the whole line would be, from the points as laid (never from smoothed ones: smoothing the
        // kept line every frame would drift it): the new points, and the two stored before the joint, whose
        // neighbours on are new
        const size_t n_old = r.dense.size();
        r.dense_s.insert(r.dense_s.end(), dense_s.begin(), dense_s.end());
        r.dense_raw.insert(r.dense_raw.end(), dense.begin(), dense.end());
        r.dense.insert(r.dense.end(), dense.begin(), dense.end());
        const size_t j0 = n_old > 2 ? n_old - 2 : 0, w0 = j0 > 2 ? j0 - 2 : 0;
        std::vector<LanePoint> work(r.dense_raw.begin() + w0, r.dense_raw.end());
        smoothLine(work, 1);
        std::copy(work.begin() + (j0 - w0), work.end(), r.dense.begin() + j0);
        for (const auto& [i, j] : w.junctions)
        {
            r.junctions.push_back({ serial[std::min(i, serial.size() - 1)], j });
        }
        for (const auto& [i, sc] : w.surface_changes)
        {
            r.surface_changes.push_back({ serial[std::min(i, serial.size() - 1)], sc });
        }
        for (const auto& [i, h] : w.signs)
        {
            StoredRoad::SignHit hit = h;
            hit.step = serial[std::min(i, serial.size() - 1)];
            r.signs.push_back(hit);
        }
        if (sView.jump && !r.jump)
        {
            r.jump = true;
            r.jump_step = serial[std::min(w.jump_step, serial.size() - 1)];
            r.jump_x = sView.jump_x;
            r.jump_y = sView.jump_y;
            r.jump_z = sView.jump_z;
            r.land_x = sView.land_x;
            r.land_y = sView.land_y;
            r.land_z = sView.land_z;
            r.jump_dx = sView.jump_dx;
            r.jump_dy = sView.jump_dy;
        }
        r.ended = w.ended;
        r.stuck = w.stuck;
        r.road_end = sView.road_end;
        r.road_end_x = sView.road_end_x;
        r.road_end_y = sView.road_end_y;
        r.road_end_dx = sView.road_end_dx;
        r.road_end_dy = sView.road_end_dy;
        r.walk_end = sView.walk_end;
        r.keep = keep;
    }

    // ---------------------------------------------------------------- where the walk may go on to

    // Nexii's rules (2026-11-07, Melanthia: the line turned off Route 5 through an arch onto SLRR Melanthia Station's
    // forecourt, mole-built asphalt tiles on the station's own parcel): the walk goes on, and a junction offers a
    // branch, only onto road. A pavement, a plaza or a forecourt is not road, Linden's or not; a parcel named for a
    // place (a station, an airport) is a destination, entered only along a road leading through it; and without
    // SKRoadAutopilotPrivateRoads the car keeps to Linden road land. Refused, the parcel line is a road end.
    constexpr F64 OPEN_AREA_M = 20.0;           // a linkset of square tiles this wide both ways is an area, not a road
    constexpr S32 OPEN_AREA_TILES = 3;

    bool pedestrianName(const std::string& raw)
    {
        std::string n = raw;
        LLStringUtil::toLower(n);
        for (const char* road : { "road", "street", "route", "highway", "avenue" })
        {
            if (n.find(road) != std::string::npos)
            {
                return false;
            }
        }
        for (const char* walk : { "pavement", "sidewalk", "footpath", "walkway", "plaza", "forecourt", "platform", "station",
                                  "pedestrian", "path" })
        {
            if (n.find(walk) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    // A linkset measured as a road kit lying as a road: a road kit's piece.
    bool kitRoad(const LLUUID& root)
    {
        return kitPlace(root).kit != nullptr;
    }

    // A linkset laid as square flat tiles over an area wider than a road both ways (a forecourt's 10 m asphalt
    // squares), or one big square slab: its tiles have no axis to walk along. Worked once per linkset per walk.
    std::unordered_map<LLUUID, bool> sOpenAreas;
    bool openArea(const LLUUID& root)
    {
        if (auto it = sOpenAreas.find(root); it != sOpenAreas.end())
        {
            return it->second;
        }
        S32 tiles = 0, others = 0;
        F64 x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const PieceShapes* p : sParts)
        {
            if (p->root != root)
            {
                continue;
            }
            F64 ax, ay, along, across;
            partAxis(*p, 1, 0, ax, ay, along, across);
            (partSquare(*p) ? tiles : others) += 1;
            const F64 h = 0.5 * std::max(along, across);
            x0 = std::min(x0, p->cx - h);
            x1 = std::max(x1, p->cx + h);
            y0 = std::min(y0, p->cy - h);
            y1 = std::max(y1, p->cy + h);
        }
        bool open = false;
        if (tiles + others > 0)
        {
            const bool wide = x1 - x0 >= OPEN_AREA_M && y1 - y0 >= OPEN_AREA_M;
            open = wide && (tiles >= OPEN_AREA_TILES || (tiles == 1 && others == 0)) && tiles >= 2 * others;
        }
        sOpenAreas[root] = open;
        return open;
    }

    // Road going on out of `part` past (x, y) along (dx, dy): a definite piece of road width ahead, not `from`.
    bool leadsOn(S32 part, S32 from, F64 x, F64 y, F64 dx, F64 dy)
    {
        for (const Continuation& c : sConts[part])
        {
            if (c.other == from || !sPartDefinite[c.other] || (c.x - x) * dx + (c.y - y) * dy < 2.0)
            {
                continue;
            }
            F64 ax, ay, along, across;
            partAxis(*sParts[c.other], dx, dy, ax, ay, along, across);
            if (across >= MIN_BRANCH_W && across < OPEN_W)
            {
                return true;
            }
        }
        return false;
    }

    // Road leading on out of a tiled area the walk enters at (x, y) along (dx, dy) from `from`: through the area's
    // own tiles (linkset of `part`), a definite piece of another linkset at road width, ahead and on the line the
    // walk came in along (a junction of square tiles between two roads), not a forecourt with no way on through.
    constexpr F64 THROUGH_AREA_LANE_M = 6.0;    // the way out within this of the line the walk came in on
    bool leadsOut(S32 part, S32 from, F64 x, F64 y, F64 dx, F64 dy)
    {
        const LLUUID& area = sParts[part]->root;
        const LLUUID& came = sParts[from]->root;
        std::array<S32, 32> todo;
        std::array<S32, 32> seen;
        S32 n_todo = 0, n_seen = 0;
        todo[n_todo++] = part;
        seen[n_seen++] = part;
        while (n_todo > 0)
        {
            const S32 at = todo[--n_todo];
            for (const Continuation& c : sConts[at])
            {
                const F64 ahead = (c.x - x) * dx + (c.y - y) * dy;
                if (c.other == from || ahead < 2.0 || std::fabs((c.x - x) * dy - (c.y - y) * dx) > THROUGH_AREA_LANE_M)
                {
                    continue;
                }
                const PieceShapes& po = *sParts[c.other];
                if (po.root == area)
                {
                    if (n_seen < S32(seen.size()) && std::find(seen.begin(), seen.begin() + n_seen, c.other) == seen.begin() + n_seen)
                    {
                        seen[n_seen++] = c.other;
                        todo[n_todo++] = c.other;
                    }
                    continue;
                }
                if (po.root == came || !sPartDefinite[c.other] || openArea(po.root))
                {
                    continue;
                }
                F64 ax, ay, along, across;
                partAxis(po, dx, dy, ax, ay, along, across);
                if (across >= MIN_BRANCH_W && across < OPEN_W)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Each refusal or allowance logged once per part and reason.
    std::unordered_set<std::string> sLandLogged;
    void logLand(const LLUUID& part, const std::string& what)
    {
        if (sLandLogged.size() > 4096)
        {
            sLandLogged.clear();
        }
        if (sLandLogged.insert(part.asString() + what).second)
        {
            LL_INFOS("SKRoadDriver") << "walk onto " << part << ": " << what << LL_ENDL;
        }
    }

    // Why the walk may not go on from `from` onto `part`, entered at (x, y) along (dx, dy); null where it may.
    // `unknown` is set where the land's owner is not in yet (no choice at a junction onto it; a sole way on stands).
    const char* landRefuses(S32 from, S32 part, F64 x, F64 y, F64 dx, F64 dy, bool& unknown)
    {
        unknown = false;
        const PieceShapes& pn = *sParts[part];
        const PieceShapes& pc = *sParts[from];
        const bool own_road = pn.root == pc.root || std::find(sChainRoots.begin(), sChainRoots.end(), pn.root) != sChainRoots.end();
        F64 ax, ay, along, across;
        partAxis(pn, dx, dy, ax, ay, along, across);
        // the strong signal: road outright, of a road kit or the road already walked, at road width, leading on
        const bool kit = kitRoad(pn.root);
        const bool strong = sPartDefinite[part] && (kit || own_road) && across < OPEN_W && leadsOn(part, from, x, y, dx, dy);
        const host::Props* props = host::properties(pn.root);
        if (props && !kit && pedestrianName(props->mName))
        {
            logLand(pn.part, "refused: pavement by its name \"" + props->mName + "\"");
            return "pavement";
        }
        if (!own_road && !strong && openArea(pn.root))
        {
            if (leadsOut(part, from, x, y, dx, dy))
            {
                logLand(pn.part, "allowed: square tiles the road goes on through (a junction, not an open area)");
                return nullptr;
            }
            logLand(pn.part, "refused: an open area of square tiles, no road kit through it");
            return "open area";
        }
        const bool private_ok = host::privateRoadsAllowed();
        for (F64 d : { 2.0, 6.0 })
        {
            std::string name;
            const S32 state = host::roadLandAt(x + dx * d, y + dy * d, &name);
            if (state == 2)
            {
                if (!strong)
                {
                    logLand(pn.part, "refused: destination parcel \"" + name + "\" (no road kit leading through)");
                    return "destination parcel";
                }
                logLand(pn.part, "allowed into destination parcel \"" + name + "\": " + (kit ? "a road kit" : "the road walked") + " leading on");
            }
            else if (state == 0 && !private_ok)
            {
                logLand(pn.part, "refused: not Linden road land, parcel \"" + name + "\"");
                return "not Linden land";
            }
            else if (state < 0 && !private_ok)
            {
                unknown = true;
            }
        }
        return nullptr;
    }

    std::vector<LanePoint> boxWalk(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 v, F64 keep)
    {
        sOpenAreas.clear();
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - boxWalk");
        forgetPassedCommit(x, y);
        sWalkParts.clear();
        const F64 now = sClock.getElapsedTimeF64();
        // where on the committed route the car is: the step of the part under it, else (on the road's other half,
        // a part beside the route) the step nearest it while that is within a lane of the route's line
        S32 under = partUnder(x, y, z, hx, hy);
        S32 at = -1;
        std::string reseed;
        if (!sCommitted.empty())
        {
            if (std::hypot(x - sRouteCarX, y - sRouteCarY) > ROUTE_TELEPORT_M && now - sRouteWalkAt < 5.0)
            {
                reseed = "teleport";
            }
            for (S32 i = S32(sCommitted.size()); reseed.empty() && i-- > 0;)
            {
                if (under >= 0 && sCommitted[i].part == sParts[under] && routeIndex(sCommitted[i]) == under)
                {
                    at = i;
                    break;
                }
            }
            if (reseed.empty() && at < 0)
            {
                F64 best = 1e18;
                for (S32 i = 0; i + 1 < S32(sCommitted.size()); ++i)
                {
                    const RouteStep &a = sCommitted[i], &b = sCommitted[i + 1];
                    const F64 ex = b.x - a.x, ey = b.y - a.y, L2 = ex * ex + ey * ey;
                    const F64 t = L2 > 1e-9 ? std::clamp(((x - a.x) * ex + (y - a.y) * ey) / L2, 0.0, 1.0) : 0.0;
                    const F64 d = std::hypot(x - a.x - t * ex, y - a.y - t * ey);
                    if (d < best && routeIndex(a) >= 0)
                    {
                        best = d;
                        at = i;
                    }
                }
                if (at >= 0 && best > ROUTE_OFF_M)
                {
                    if (sOffRouteSince < 0)
                    {
                        sOffRouteSince = now;
                    }
                    if (now - sOffRouteSince > ROUTE_OFF_S || under < 0)
                    {
                        reseed = llformat("off the route %.1f m for %.1f s", best, now - sOffRouteSince);
                        at = -1;
                    }
                }
                else if (at < 0)
                {
                    reseed = "route gone";
                }
            }
            if (at >= 0 && reseed.empty())
            {
                // turned round: the car facing back along the route (a recovery that turned it deliberately); the
                // route's way where it passes nearest the car
                F64 rdx = 0, rdy = 0, nearest = 1e18;
                for (S32 i = 0; i + 1 < S32(sCommitted.size()); ++i)
                {
                    const RouteStep &a = sCommitted[i], &b = sCommitted[i + 1];
                    const F64 ex = b.x - a.x, ey = b.y - a.y, L2 = ex * ex + ey * ey;
                    const F64 t = L2 > 1e-9 ? std::clamp(((x - a.x) * ex + (y - a.y) * ey) / L2, 0.0, 1.0) : 0.0;
                    const F64 d = std::hypot(x - a.x - t * ex, y - a.y - t * ey);
                    if (d < nearest && L2 > 1.0)
                    {
                        nearest = d;
                        rdx = ex;
                        rdy = ey;
                    }
                }
                const F64 rl = std::hypot(rdx, rdy);
                const F64 cosine = rl > 1.0 ? (rdx * hx + rdy * hy) / rl : 1.0;
                if (cosine < -0.5)
                {
                    reseed = "turned round";
                    at = -1;
                }
                else if (cosine < std::cos(ROUTE_TURNED_DEG * PI / 180))
                {
                    // turned off it: the car's heading away from the route's way for a while (a turn taken at a
                    // junction the route goes straight on through, a recovery's turn)
                    if (sTurnedSince < 0)
                    {
                        sTurnedSince = now;
                    }
                    if (now - sTurnedSince > ROUTE_TURNED_S)
                    {
                        reseed = llformat("turned off it (%.0f deg for %.1f s)", std::acos(cosine) * 180 / PI, now - sTurnedSince);
                        at = -1;
                    }
                }
                else
                {
                    sTurnedSince = -1;
                }
            }
        }
        sRouteCarX = x;
        sRouteCarY = y;
        sRouteWalkAt = now;
        if (at >= 0 && under >= 0 && sCommitted[at].part == sParts[under])
        {
            sOffRouteSince = -1;    // on the route's own part: the off-route clock stops
        }
        S32 cur = at >= 0 ? routeIndex(sCommitted[at]) : under;
        if (cur < 0)
        {
            return {};
        }
        if (at < 0)
        {
            sView.route = sCommitted.empty() ? "seeded" : "seeded afresh: " + (reseed.empty() ? std::string("off the route") : reseed);
            // a reseed for the reason last said is counted, not said again, for ROUTE_SAY_GAP_S
            static std::string route_said;
            static F64 route_said_at = -1e9;
            static S32 route_unsaid = 0;
            constexpr F64 ROUTE_SAY_GAP_S = 10.0;
            if (!sCommitted.empty())
            {
                if (sView.route != route_said || now - route_said_at > ROUTE_SAY_GAP_S)
                {
                    LL_INFOS("SKRoadDriver") << "road route " << sView.route
                                             << (route_unsaid > 0 ? llformat(" (%d more reseeds since the last line)", route_unsaid) : std::string())
                                             << LL_ENDL;
                    route_said = sView.route;
                    route_said_at = now;
                    route_unsaid = 0;
                }
                else
                {
                    ++route_unsaid;
                }
            }
            sCommitted.clear();
            sOffRouteSince = -1;
            sTurnedSince = -1;
        }
        const F64 length = lookAhead(v) + 10.0;
        // The stored road (StoredRoad): where the car is along it, what changed under it, and from where the walk
        // goes on. What was walked stands: the walk resumes at its far end when the car needs more road ahead, or at
        // the first step a change touched, and from the car only where the car's own step is the last stored one
        // (or nothing is stored). A walk afresh (the repair below) works as before, from the car, storing nothing.
        const bool storing = sRepairDepth == 0;
        S32 resume = -1;            // the committed step the walk resumes at (its entry), else it starts at the car
        S32 k_at = -1;              // the car's step in the stored road
        size_t k0 = 0;              // the stored step the walk's first step is
        F64 s0 = 0;                 // where along the stored road the walk starts
        std::string store_why;      // what changed, where it re-lays the stored road
        if (storing)
        {
            if (at < 0 || sStoreFolded)
            {
                sStored.clear();
                sStoreFolded = false;
            }
            k_at = at >= 0 ? storeStep(sCommitted[at].id) : -1;
            if (k_at < 0 && !sStored.empty())
            {
                LL_INFOS("SKRoadDriver") << "road store: dropped, the car's route step is not on it" << LL_ENDL;
                sStored.clear();
            }
            if (!sStored.empty())
            {
                storeLocate(x, y);
                storeDropBehind();
                k_at = storeStep(sCommitted[at].id);
            }
            if (k_at >= 0 && !sStored.empty())
            {
                std::string why;
                S64 hit = storeSeen(true, why);
                if (std::fabs(keep - sStored.keep) > 1e-3)
                {
                    hit = S64(sStored.car_i);
                    why += "the lane's keep changed ";
                }
                const S32 k_hit = hit >= 0 ? storeStepAt(sStored.dense_s[size_t(hit)]) : -1;
                if (!why.empty())
                {
                    store_why = why;
                    LL_DEBUGS("SKRoad") << "road store: " << why
                                             << (k_hit < 0 ? std::string("- nothing ahead re-laid")
                                                           : llformat("- re-laid from step %d of %d (part %s, %.0f m ahead of the car)", k_hit,
                                                                      S32(sStored.steps.size()), sStored.steps[k_hit].part.asString().substr(0, 8).c_str(),
                                                                      sStored.steps[k_hit].s - sStored.car_s)) << LL_ENDL;
                }
                // the stored steps from the car on are the route's (they were walked along it): the last one still met
                // by the route, at or before the first a change touched
                auto committed = [&](S32 k)
                {
                    const S32 ci = at + (k - k_at);
                    return ci >= 0 && ci < S32(sCommitted.size()) && sCommitted[ci].id == sStored.steps[k].part && routeIndex(sCommitted[ci]) >= 0
                               ? ci : -1;
                };
                S32 last = k_hit >= 0 ? k_hit : S32(sStored.steps.size()) - 1;
                while (last > k_at && committed(last) < 0)
                {
                    --last;
                }
                const F64 ahead = sStored.dense_s.back() - sStored.car_s;
                const U32 car_serial = sStored.steps[k_at].serial;
                const bool retry_stuck = sStored.stuck && (sStored.stuck_parts_gen != sPartsGen || sStored.stuck_verdict_gen != sVerdictGen
                                                           || sStored.stuck_car_step != car_serial);
                if (last > k_at && (k_hit >= 0 || (!sStored.ended && ahead < length) || retry_stuck))
                {
                    resume = committed(last);
                    k0 = size_t(last);
                    s0 = sStored.steps[last].s;
                    storeTruncate(k0, s0);
                }
                else if (last > k_at)
                {
                    // nothing to walk: the stored road reaches far enough (or ends) and nothing under it changed; the
                    // route's history is trimmed as a walk trims it
                    S32 from = at;
                    for (F64 back = 0; from > 0 && back < ROUTE_HISTORY_M; --from)
                    {
                        back += std::hypot(sCommitted[from].x - sCommitted[from - 1].x, sCommitted[from].y - sCommitted[from - 1].y);
                    }
                    sCommitted.erase(sCommitted.begin(), sCommitted.begin() + from);
                    at -= from;
                    sLastTravelled = ahead;
                    sView.route = llformat("stored: %.0f m ahead, nothing walked", ahead);
                    return storeOut(x, y, at, length);
                }
            }
            if (resume < 0)
            {
                if (k_at >= 0 && !sStored.empty())
                {
                    k0 = size_t(k_at);
                    s0 = sStored.car_s;
                    storeTruncate(k0, s0);
                }
                else
                {
                    sStored.clear();
                }
            }
            else
            {
                cur = routeIndex(sCommitted[resume]);
            }
        }
        const bool from_car = resume < 0;
        static std::vector<U32> visited;
        static U32 walk = 0;
        ++walk;
        if (visited.size() < sParts.size())
        {
            visited.resize(sParts.size(), 0);
        }
        // the way travelled: not walked back onto (and where the walk resumes, the stored road walked already)
        for (S32 i = 0; i < (from_car ? at : resume); ++i)
        {
            const S32 k = routeIndex(sCommitted[i]);
            if (k >= 0)
            {
                visited[k] = walk;
            }
        }
        visited[cur] = walk;
        if (from_car)
        {
            sWalkParts.push_back(cur);
            sChainRoots.assign(1, sParts[cur]->root);
        }
        else
        {
            sChainRoots.clear();
            for (S32 i = at; i <= resume; ++i)
            {
                const S32 k = routeIndex(sCommitted[i]);
                if (k >= 0)
                {
                    sWalkParts.push_back(k);
                    if (sChainRoots.empty() || sChainRoots.back() != sParts[k]->root)
                    {
                        sChainRoots.push_back(sParts[k]->root);
                    }
                }
            }
        }
        F64 dx, dy, along, across;
        partAxis(*sParts[cur], hx, hy, dx, dy, along, across);
        F64 rdx = 0, rdy = 0;       // the way the route runs through the part, where it runs on from it or into it
        if (!from_car)
        {
            const RouteStep& e = sCommitted[resume];
            if (e.dx != 0 || e.dy != 0)
            {
                dx = e.dx;
                dy = e.dy;
            }
        }
        else if (at >= 0 && sCommitted.size() > 1)
        {
            const RouteStep& s = sCommitted[at];
            rdx = at + 1 < S32(sCommitted.size()) ? sCommitted[at + 1].x - s.x : s.x - sCommitted[at - 1].x;
            rdy = at + 1 < S32(sCommitted.size()) ? sCommitted[at + 1].y - s.y : s.y - sCommitted[at - 1].y;
            const F64 rl = std::hypot(rdx, rdy);
            rdx = rl > 1e-6 ? rdx / rl : 0;
            rdy = rl > 1e-6 ? rdy / rl : 0;
            if (rl > 1e-6 && !partSquare(*sParts[cur]))
            {
                partAxis(*sParts[cur], rdx, rdy, dx, dy, along, across);
            }
        }
        if (!from_car)
        {
            // resumed: the way the step was entered by
        }
        else if (partSquare(*sParts[cur]) && (rdx != 0 || rdy != 0))
        {
            dx = rdx;   // a square part's way: the route's through it
            dy = rdy;
        }
        else if (partSquare(*sParts[cur]))
        {
            // a square part has no way of its own: either axis could be the road's, and on a diagonal heading the
            // one across it was taken (the road ahead then read as behind or beside, and the walk turned 90 deg
            // into a rail): the road's way as the last good line ran (the car's heading laid the line wherever the
            // car pointed, and the car, steering after it, turned circles), else the car's heading
            if (sNow - sLineDirAt < LINE_DIR_KEEP_S)
            {
                dx = sLineDirX;
                dy = sLineDirY;
            }
            else
            {
                dx = hx;
                dy = hy;
            }
        }
        // the first point: abreast of the car on the part's centre line; where the walk resumes, the stored line's
        // last points, ending at the step's entry
        std::vector<PartPoint> line;
        F64 px, py;
        if (from_car)
        {
            const PieceShapes& p0 = *sParts[cur];
            const F64 a = (x - p0.cx) * dx + (y - p0.cy) * dy;
            px = p0.cx + a * dx;
            py = p0.cy + a * dy;
            line.push_back({ px, py, cur, cur });
        }
        else
        {
            std::unordered_map<LLUUID, S32> index;
            for (S32 i = 0; i <= resume; ++i)
            {
                index[sCommitted[i].id] = routeIndex(sCommitted[i]);
            }
            auto of = [&](const LLUUID& id)
            {
                auto f = index.find(id);
                return f != index.end() && f->second >= 0 ? f->second : cur;
            };
            const std::vector<StoredRoad::Point>& sl = sStored.line;
            for (size_t i = sl.size() > 3 ? sl.size() - 3 : 0; i < sl.size(); ++i)
            {
                line.push_back({ sl[i].x, sl[i].y, of(sl[i].a), of(sl[i].b) });
            }
            line.back().b = cur;
            px = sCommitted[resume].x;
            py = sCommitted[resume].y;
            line.back().x = px;
            line.back().y = py;
        }
        const size_t line_base = line.size();
        // how far along from the car the walk starts, and the car's place along the stored road
        const F64 s_car = !storing ? 0.0 : from_car ? s0 : sStored.car_s;
        F64 travelled = from_car ? 0.0 : s0 - s_car;
        // the route from the car on: the step it is on, then each part entered
        std::vector<RouteStep> walked;
        std::vector<size_t> walked_line;    // each step's entry in line
        if (!from_car)
        {
            walked.assign(sCommitted.begin() + at, sCommitted.begin() + resume + 1);
            walked_line.assign(walked.size(), 0);
            walked_line.back() = line_base - 1;
        }
        else if (at >= 0)
        {
            walked.push_back(sCommitted[at]);
            walked_line.push_back(0);
        }
        else
        {
            RouteStep seed;
            seed.part = sParts[cur];
            seed.id = sParts[cur]->part;
            seed.x = px;
            seed.y = py;
            seed.dx = dx;
            seed.dy = dy;
            walked.push_back(seed);
            walked_line.push_back(0);
        }
        const size_t prefilled = walked.size();
        // what the walk meets, by the walked step it is met on (for the stored road)
        std::vector<std::pair<size_t, Junction>> met_junctions;
        std::vector<std::pair<size_t, View::SurfaceChange>> met_surface;
        std::vector<std::pair<size_t, StoredRoad::SignHit>> met_signs;
        size_t jump_step = 0;
        bool ended = false;
        if (storing && !from_car)
        {
            // the stored road's junctions and jump ahead of the car, for the walk on (and its lane's widths)
            const U32 car_serial = sStored.steps[k_at].serial;
            for (const auto& [step, j] : sStored.junctions)
            {
                if (step >= car_serial)
                {
                    sView.junctions.push_back(j);
                }
            }
            sView.jump = sStored.jump && sStored.jump_step >= car_serial;
        }
        // the way travelled into (px, py): back along the line, then the route's history; false where too little
        auto incoming = [&](F64& ix, F64& iy)
        {
            F64 got = 0, qx = px, qy = py, fx = px, fy = py;
            auto take = [&](F64 ox, F64 oy)
            {
                if (got >= ROUTE_HEADING_M)
                {
                    return;
                }
                got += std::hypot(qx - ox, qy - oy);
                qx = fx = ox;
                qy = fy = oy;
            };
            for (size_t k = line.size(); k-- > 0;)
            {
                take(line[k].x, line[k].y);
            }
            for (S32 k = from_car ? at : resume; k >= 0 && got < ROUTE_HEADING_M; --k)
            {
                take(sCommitted[k].x, sCommitted[k].y);
            }
            const F64 L = std::hypot(px - fx, py - fy);
            if (L < 4.0)
            {
                return false;
            }
            ix = (px - fx) / L;
            iy = (py - fy) / L;
            return true;
        };
        // on into the way chosen: the line through the part to where it leaves, and the step taken
        auto advance = [&](const Branch& go, bool junction)
        {
            const PieceShapes& pc = *sParts[cur];
            const PieceShapes& pn = *sParts[go.seg];
            {
                // its middle between where the line came in and where it leaves: a box's centre, on its centre
                // line; otherwise halfway (measured across, below)
                const F64 mx = pc.box ? pc.cx : (px + go.x) / 2, my = pc.box ? pc.cy : (py + go.y) / 2;
                const F64 cx = go.x - px, cy = go.y - py, L2 = cx * cx + cy * cy;
                const F64 t = L2 > 1e-6 ? ((mx - px) * cx + (my - py) * cy) / L2 : -1;
                if (t > 0.05 && t < 0.95)
                {
                    line.push_back({ mx, my, cur, cur });
                }
            }
            if (pc.top_known && pn.top_known && pc.top_tex != pn.top_tex && !sameKit(pc.root, pn.root))
            {
                // the surface changes: the same road re-surfaced where it goes on as wide and in line
                F64 a1, a2, l1, w1, l2, w2;
                partAxis(pc, dx, dy, a1, a2, l1, w1);
                partAxis(pn, go.dx, go.dy, a1, a2, l2, w2);
                const F64 ratio = w2 / std::max(0.1, w1);
                sView.surface_changes.push_back({ go.x, go.y, pc.zt, go.dx, go.dy, w2,
                                                  ratio > 1 / SAME_ROAD_WIDTH && ratio < SAME_ROAD_WIDTH && std::fabs(go.turn_deg) < 10.0 });
                met_surface.push_back({ walked.size() - 1, sView.surface_changes.back() });
            }
            line.push_back({ go.x, go.y, cur, go.seg });
            travelled += std::hypot(go.x - px, go.y - py);
            visited[go.seg] = walk;
            sWalkParts.push_back(go.seg);
            if (sChainRoots.back() != go.root)
            {
                sChainRoots.push_back(go.root);
            }
            cur = go.seg;
            px = go.x;
            py = go.y;
            dx = go.dx;
            dy = go.dy;
            RouteStep st;
            st.part = &pn;
            st.id = pn.part;
            st.x = go.x;
            st.y = go.y;
            st.dx = go.dx;
            st.dy = go.dy;
            st.has_junction = junction;
            walked.push_back(std::move(st));
            walked_line.push_back(line.size() - 1);
        };
        bool stuck = false;     // the walk ended short of the road's end with a route to follow
        size_t walked_n = 0;
        // the committed route ahead, followed while each part on it is still a way on from the last (road, there,
        // met): where the route goes straight on from a part, taken as it is; where it was a junction, the ways on
        // are read again (what the view reports is the junction as it is now) and the one chosen before is taken
        // while it is still among them. Where a part is no longer a way on, the route is repaired from there.
        S32 follow = from_car ? at : resume;    // the committed step the walk is on, while it follows the route
        S32 followed = 0;
        std::string broke;      // why the route stopped being followed short of its end
        auto stopFollowing = [&](const char* why)
        {
            if (follow >= 0 && follow + 1 < S32(sCommitted.size()))
            {
                broke = llformat("repaired from part %s (%s)", sCommitted[follow + 1].id.asString().substr(0, 8).c_str(), why);
            }
            follow = -1;
        };
        // the committed step `nx` as a way on from the part the walk is on: false (and why) where it is not one now
        auto routeWay = [&](const RouteStep& nx, Branch& go, const char*& why)
        {
            const S32 k = routeIndex(nx);
            const Continuation* best = nullptr;
            F64 bd = 1e18;
            if (k >= 0 && visited[k] != walk && (sPartDefinite[k] || sPartPending[k]))
            {
                for (const Continuation& c : sConts[cur])
                {
                    const F64 d = std::hypot(c.x - nx.x, c.y - nx.y);
                    if (c.other == k && d < bd)
                    {
                        bd = d;
                        best = &c;
                    }
                }
                for (size_t h = 0; !best && h < sConts[cur].size(); ++h)
                {
                    // or on through the road's other half alongside, as the walk goes where this half meets nothing
                    // on (see halves below)
                    for (const Continuation& c2 : sConts[sConts[cur][h].other])
                    {
                        const F64 d = std::hypot(c2.x - nx.x, c2.y - nx.y);
                        if (c2.other == k && d < bd)
                        {
                            bd = d;
                            best = &c2;
                        }
                    }
                }
            }
            if (!best)
            {
                why = k < 0 ? "gone" : visited[k] == walk ? "walked back onto" : !(sPartDefinite[k] || sPartPending[k]) ? "not road" : "no longer met";
                return false;
            }
            const PieceShapes& pn = *sParts[k];
            const TopEdge& eb = pn.top[best->other_edge];
            go.seg = k;
            go.root = pn.root;
            go.part = pn.part;
            go.x = best->x;
            go.y = best->y;
            go.dx = -eb.ox;
            go.dy = -eb.oy;
            go.definite = sPartDefinite[k];
            go.on_route = true;
            go.turn_deg = std::atan2(dx * go.dy - dy * go.dx, go.dx * dx + go.dy * dy) * 180 / PI;
            return true;
        };
        // the road's end by name on the part the walk is on: a piece named as the end, or a sign saying the road
        // ends standing on or beside it ahead. Where it ends (from the walk's point on), and why; false: no end here.
        auto namedEnd = [&](F64& through, std::string& why)
        {
            const PieceShapes& pc = *sParts[cur];
            const F64 reach_m = farSide(pc, px, py, dx, dy);
            const bool first = from_car && line.size() == 1;
            if (endPieceByName(pc.root))
            {
                // on the car's own part, a cap the car is leaving: road met ahead of it is the way on
                bool on = false;
                for (const Continuation& c : sConts[cur])
                {
                    on |= first && (sPartDefinite[c.other] || sPartPending[c.other]) && (c.x - px) * dx + (c.y - py) * dy > 0.5;
                }
                if (!on)
                {
                    through = reach_m;
                    why = "end piece \"" + host::properties(pc.root)->mName + "\"";
                    return true;
                }
            }
            F64 best = 1e18;
            for (const SignCandidate& s : sSignCandidates)
            {
                if (s.root == pc.root || s.z0 > pc.zt + END_SIGN_Z_M || s.z1 < pc.zt - END_SIGN_Z_M)
                {
                    continue;
                }
                const F64 gx = std::max({ pc.bx0 - s.x, 0.0, s.x - pc.bx1 }), gy = std::max({ pc.by0 - s.y, 0.0, s.y - pc.by1 });
                const F64 along = (s.x - px) * dx + (s.y - py) * dy;
                // ahead of the car (behind it once it has turned round there), and no further than the part reaches
                if (std::hypot(gx, gy) > END_SIGN_BESIDE_M || along < (first ? 1.0 : -2.0) || along > reach_m + 1.0 || along >= best)
                {
                    continue;
                }
                // a posted limit or a warning ahead: the nearest along the walk, for the driver's speed
                if (const Sign* sign = signByTexture(s.root))
                {
                    if ((sign->what == SignClass::SpeedLimit && sign->value > 0.f)
                        || sign->what == SignClass::JunctionWarning || sign->what == SignClass::CurveWarning || sign->what == SignClass::Danger
                        || sign->what == SignClass::Stop || sign->what == SignClass::GiveWay)
                    {
                        met_signs.push_back({ walked.size() - 1, { 0, s_car + travelled + along, S32(sign->what), sign->value } });
                    }
                    if (sign->what == SignClass::SpeedLimit && sign->value > 0.f && sView.sign_speed_limit <= 0.f)
                    {
                        sView.sign_speed_limit = sign->value;
                        sView.sign_speed_at = travelled + along;
                    }
                    else if (sView.sign_warning == 0
                             && (sign->what == SignClass::JunctionWarning || sign->what == SignClass::CurveWarning || sign->what == SignClass::Danger
                                 || sign->what == SignClass::Stop || sign->what == SignClass::GiveWay))
                    {
                        sView.sign_warning = S32(sign->what);
                        sView.sign_warning_at = travelled + along;
                    }
                }
                // an unofficial sign only on the deck or at its edge; the LDPW's, and one whose texture says it, within
                // END_SIGN_BESIDE_M
                const S32 kind = endSignByName(s.root);
                if (kind >= 2 || (kind == 1 && std::hypot(gx, gy) < 1.0))
                {
                    // Nexii's rule (2026-11-07, Richfield and Japan Plaza): a marker ends only the road running into
                    // it: on that road's own width ahead, and a rail or a sign's panel square to it; a road running
                    // past it, the marker beside its edge, is not ended. Where a way on turns off before the marker
                    // (the side road's end at a T), the marker says turn there, not a dead end.
                    F64 rax, ray, ralong, racross;
                    partAxis(pc, dx, dy, rax, ray, ralong, racross);
                    const F64 lateral = std::fabs((s.x - pc.cx) * ray - (s.y - pc.cy) * rax);
                    if (lateral > 0.5 * racross + END_SIGN_MARGIN_M || (s.oriented && std::fabs(s.lx * rax + s.ly * ray) > END_SIGN_ACROSS_SIN))
                    {
                        continue;
                    }
                    bool turns = false;
                    for (const Continuation& c : sConts[cur])
                    {
                        const F64 at = (c.x - px) * dx + (c.y - py) * dy;
                        turns |= (sPartDefinite[c.other] || sPartPending[c.other]) && visited[c.other] != walk && at > (first ? 0.5 : -2.0)
                                 && at < along;
                    }
                    if (turns)
                    {
                        continue;
                    }
                    best = along;
                    const host::Props* props = host::properties(s.root);
                    const Sign* sign = kind == 3 ? signByTexture(s.root) : nullptr;
                    why = llformat("sign \"%s\"%s", props ? props->mName.c_str() : s.root.asString().c_str(),
                                   kind == 3 ? (" (reads \"" + sign->text + "\")").c_str() : kind == 2 ? " (Linden)" : "");
                }
            }
            if (best < 1e17)
            {
                through = std::clamp(best, 0.0, reach_m);
                return true;
            }
            return false;
        };
        while (travelled < length)
        {
            F64 through = 0;
            std::string why_end;
            if (namedEnd(through, why_end))
            {
                sView.walk_end = llformat("%s%s on part %s after %.0f m", WALK_END_ROAD_END, why_end.c_str(),
                                          sParts[cur]->part.asString().substr(0, 8).c_str(), travelled);
                // the road ends here by name: the line stops there, straight, with no way on taken past it (no jump,
                // no turn into what lies beyond)
                const F64 ex = px + through * dx, ey = py + through * dy;
                if (through > 0.05)
                {
                    line.push_back({ ex, ey, cur, cur });
                }
                sView.road_end = true;
                sView.road_end_x = ex;
                sView.road_end_y = ey;
                sView.road_end_dx = dx;
                sView.road_end_dy = dy;
                ended = true;
                break;
            }
            if (follow >= 0 && follow + 1 < S32(sCommitted.size()) && !sCommitted[follow + 1].has_junction && !sCommitted[follow + 1].jump)
            {
                Branch go;
                const char* why = "";
                if (routeWay(sCommitted[follow + 1], go, why))
                {
                    advance(go, false);
                    ++follow;
                    ++followed;
                    continue;
                }
                stopFollowing(why);
            }
            const PieceShapes& pc = *sParts[cur];
            std::vector<Branch> ways;
            S32 why_visited = 0, why_not_road = 0, why_behind = 0, why_beside = 0, why_strip = 0, why_turn = 0, why_ground = 0, why_land = 0;
            const bool on_surface = ofSurface(*sParts[cur]);
            std::vector<S32> halves;        // the road's other half alongside (the beside meetings)
            // the meetings beside, tried again as ways on where only stubs lead on (a T: the road ends at a cross road
            // and the walk ended on its far kerb with no line; the car rolled into the wall)
            std::vector<const Continuation*> beside_conts;
            auto consider = [&](const Continuation& c, bool side_pass)
            {
                // behind: on the part the car is on, only a meeting behind its middle, or more than 3 m behind the
                // car (where the car has run up to its end, the meeting on ahead is beside or just behind the car,
                // and with it "behind" the walk ended after 0 m and the line vanished for a second or two)
                const bool first_part = from_car && line.size() == 1;
                const bool behind = first_part ? ((c.x - pc.cx) * dx + (c.y - pc.cy) * dy <= 0 || (c.x - px) * dx + (c.y - py) * dy < -3.0)
                                               : (c.x - px) * dx + (c.y - py) * dy < 0.5;
                if (visited[c.other] == walk || (!sPartDefinite[c.other] && !sPartPending[c.other]) || behind)
                {
                    why_visited += visited[c.other] == walk;
                    why_not_road += visited[c.other] != walk && !sPartDefinite[c.other] && !sPartPending[c.other];
                    why_behind += visited[c.other] != walk && (sPartDefinite[c.other] || sPartPending[c.other]);
                    return;       // been there, not road, or behind (or beside where the line came in)
                }
                const PieceShapes& pn = *sParts[c.other];
                const TopEdge& ea = pc.top[c.edge];
                const TopEdge& eb = pn.top[c.other_edge];
                const F64 bdx = -eb.ox, bdy = -eb.oy;      // on into it, square to the edge it is entered by
                {
                    // the road's other lane or half alongside: a long part, parallel, met along a side running our
                    // way, and lying more beside this part's middle than ahead of it (by where they meet, which a
                    // square part's ambiguous axes cannot mislead)
                    const F64 el = std::hypot(ea.x1 - ea.x0, ea.y1 - ea.y0);
                    F64 nax, nay, nalong, nacross;
                    partAxis(pn, dx, dy, nax, nay, nalong, nacross);
                    const F64 ahead = (c.x - pc.cx) * dx + (c.y - pc.cy) * dy;
                    const F64 aside_m = std::fabs(-(c.x - pc.cx) * dy + (c.y - pc.cy) * dx);
                    if (!side_pass && el > 1e-6 && std::fabs(((ea.x1 - ea.x0) * dx + (ea.y1 - ea.y0) * dy) / el) > 0.7 && !partSquare(pn)
                        && nax * dx + nay * dy > std::cos(SIBLING_DEG * PI / 180) && aside_m > std::fabs(ahead))
                    {
                        ++why_beside;
                        halves.push_back(c.other);
                        beside_conts.push_back(&c);
                        return;
                    }
                }
                F64 bax, bay, balong, bacross;
                partAxis(pn, bdx, bdy, bax, bay, balong, bacross);
                if (bacross < MIN_BRANCH_W)
                {
                    ++why_strip;
                    return;       // a strip, a catseye
                }
                if (bacross < turnMinBranchW() && std::fabs(std::atan2(dx * bdy - dy * bdx, bdx * dx + bdy * dy)) * 180 / PI >= TURN_NARROW_DEG)
                {
                    ++why_strip;
                    return;       // a turn onto a footpath or a driveway: no lane the car fits
                }
                if (!side_pass && !partSquare(pn) && std::fabs(bdx * dx + bdy * dy) < 0.5)
                {
                    // a long part lying along the way, entered across its side wherever the meeting lies: laid on
                    // the road or beside it, never a side road. An unnamed LDPW slab over Dorje's east seam (8.8 by
                    // 28 m along Route 8, 1 cm thick) was met 92 deg off the way; explore took it, undriven, and the
                    // line hooked back up it to the region edge, where it was cut.
                    F64 nax, nay, nalong, nacross;
                    partAxis(pn, dx, dy, nax, nay, nalong, nacross);
                    if (nalong > nacross && nax * dx + nay * dy > std::cos(SIBLING_DEG * PI / 180))
                    {
                        ++why_beside;
                        halves.push_back(c.other);
                        beside_conts.push_back(&c);
                        return;
                    }
                }
                if (on_surface && !ofSurface(*sParts[c.other]) && !leadsToSurface(cur, c.other, c.x, c.y, bdx, bdy))
                {
                    ++why_ground;
                    return;       // past the road's end: ground
                }
                bool land_unknown = false;
                if (landRefuses(cur, c.other, c.x, c.y, bdx, bdy, land_unknown))
                {
                    ++why_land;
                    return;       // a pavement, an open area, a destination or land not Linden's: no way on
                }
                if (sPartPending[c.other] && sAskFirstSet.insert(pn.root).second)
                {
                    sAskFirst.push_back(pn.root);   // the road ahead, its name not here yet: asked first
                }
                Branch b;
                b.land_unknown = land_unknown;
                b.seg = c.other;
                b.root = pn.root;
                b.part = pn.part;
                b.x = c.x;
                b.y = c.y;
                b.dx = bdx;
                b.dy = bdy;
                b.same_surface = !pc.top_known || !pn.top_known || pc.top_tex == pn.top_tex;
                b.definite = sPartDefinite[c.other];
                b.on_route = sRoute.count(pn.root) > 0;
                b.turn_deg = std::atan2(dx * bdy - dy * bdx, bdx * dx + bdy * dy) * 180 / PI;
                b.side = side_pass;
                if (std::fabs(b.turn_deg) <= MAX_BRANCH_TURN_DEG)
                {
                    ways.push_back(b);
                }
                else
                {
                    ++why_turn;
                }
            };
            for (const Continuation& c : sConts[cur])
            {
                consider(c, false);
            }
            // a way on that is a stub: a short piece meeting nothing further on (a cross road's far kerb)
            auto stub = [&](const Branch& b)
            {
                if (b.seg < 0 || b.seg >= S32(sParts.size()))
                {
                    return false;
                }
                F64 ax, ay, along, across;
                partAxis(*sParts[b.seg], b.dx, b.dy, ax, ay, along, across);
                if (along >= LONE_PIECE_M)
                {
                    return false;
                }
                for (const Continuation& c : sConts[b.seg])
                {
                    if (c.other != cur && visited[c.other] != walk && (sPartDefinite[c.other] || sPartPending[c.other])
                        && (c.x - b.x) * b.dx + (c.y - b.y) * b.dy > 0.5)
                    {
                        return false;
                    }
                }
                return true;
            };
            // nothing ahead but road met beside, other roads' on both sides (a side road's mouth at a T: the
            // cross road's pieces lie along both sides of the mouth piece, Klens 2026-11-07, the walk ended there
            // and the car shot across): the meetings beside are tried as ways on
            bool left_side = false, right_side = false;
            for (const Continuation* c : beside_conts)
            {
                const LLUUID& r_ = sParts[c->other]->root;
                if (r_ == pc.root || std::find(sChainRoots.begin(), sChainRoots.end(), r_) != sChainRoots.end())
                {
                    continue;       // the road's own other half, not a cross road
                }
                const F64 s_ = -(c->x - pc.cx) * dy + (c->y - pc.cy) * dx;
                left_side |= s_ > 0.5;
                right_side |= s_ < -0.5;
            }
            if (ways.empty() && left_side && right_side && (!from_car || line.size() > 1))
            {
                const std::vector<const Continuation*> tried = beside_conts;
                for (const Continuation* c : tried)
                {
                    consider(*c, true);
                }
            }
            else if (!ways.empty() && std::all_of(ways.begin(), ways.end(), [&](const Branch& b) { return b.root == pc.root && stub(b); }))
            {
                // nothing leads on but stubs of this part's own linkset (a cross road's far kerb): the parts of it met
                // beside are its ways on (a T), its near-square sections read as the road's other half alongside
                for (const Continuation* c : beside_conts)
                {
                    if (sParts[c->other]->root == pc.root)
                    {
                        consider(*c, true);
                    }
                }
            }
            if (ways.size() > 1)
            {
                // more than one way on: each one's turn against the way travelled, not this part's own axis (a
                // junction's cross piece, entered from its side, read the road straight on as 22 deg off the way it
                // was entered by, and the line bent 62 deg across the junction)
                F64 ix, iy;
                if (incoming(ix, iy) && ix * dx + iy * dy < std::cos(INCOMING_DEG * PI / 180))
                {
                    for (Branch& b : ways)
                    {
                        b.turn_deg = std::atan2(ix * b.dy - iy * b.dx, b.dx * ix + b.dy * iy) * 180 / PI;
                    }
                }
            }
            std::sort(ways.begin(), ways.end(), [](const Branch& a, const Branch& b) { return branchScore(a) > branchScore(b); });
            auto aside = [&](const Branch& b) { return std::fabs(-(b.x - px) * dy + (b.y - py) * dx); };
            std::vector<Branch> branches;
            for (const Branch& b : ways)
            {
                auto same = std::find_if(branches.begin(), branches.end(),
                                         [&b](const Branch& o) { return std::fabs(o.turn_deg - b.turn_deg) < SAME_BRANCH_DEG; });
                if (same == branches.end())
                {
                    branches.push_back(b);
                }
                else if (aside(b) < aside(*same))
                {
                    *same = b;
                }
            }
            // a road does not branch into itself: the linkset the car is on, met again across it (a region
            // crossing read the road's own halves as a crossroads, right and left both itself), is no way on
            branches.erase(std::remove_if(branches.begin(), branches.end(),
                                          [&](const Branch& b) { return !b.side && std::fabs(b.turn_deg) > 45.0 && (b.root == pc.root || std::find(sChainRoots.begin(), sChainRoots.end(), b.root) != sChainRoots.end()); }),
                           branches.end());
            // ways on whose pieces both lie ahead (their middles within AHEAD_DEG of the way in) are one road, not a
            // junction: the straightest stands (a curved piece's own axis read a bend's next piece as a 93 deg turn
            // off a JP road, a fake junction in 160 of 196 steps at Peacock Park)
            // a left and a right onto the same linkset, which reaches no further across the way in than about two
            // roads' width, is the piece the road crosses, not a cross street (a square slab at a region seam gave
            // both ways across as branches, a fake junction in 55 of 70 steps)
            if (branches.size() > 2)
            {
                auto reach_across = [&](const LLUUID& root)
                {
                    F64 lo = 1e18, hi = -1e18;
                    for (const PieceShapes* q : sParts)
                    {
                        if (q->root != root)
                        {
                            continue;
                        }
                        for (const Shape& sh : q->shapes)
                        {
                            for (S32 k = 0; k < sh.n; ++k)
                            {
                                const F64 a2 = -(sh.x[k] - px) * dy + (sh.y[k] - py) * dx;
                                lo = std::min(lo, a2);
                                hi = std::max(hi, a2);
                            }
                        }
                    }
                    return hi > lo ? hi - lo : 0.0;
                };
                std::vector<Branch> kept;
                for (const Branch& b : branches)
                {
                    bool pair = false;
                    for (const Branch& o : branches)
                    {
                        pair |= !b.side && &o != &b && o.root == b.root && b.turn_deg * o.turn_deg < 0 && std::fabs(b.turn_deg) > 60
                                && std::fabs(o.turn_deg) > 60;
                    }
                    if (!(pair && reach_across(b.root) < CROSSED_PIECE_M))
                    {
                        kept.push_back(b);
                    }
                }
                if (!kept.empty())
                {
                    branches.swap(kept);
                }
            }
            if (branches.size() > 1)
            {
                // a side way on is a road only where it goes on: a lone short piece beside the road (a kerb prim,
                // a wall top, an arch's top) that meets nothing further is no branch, and bent the line 90 deg
                // onto it for a frame. The way straight on is held to it too where a T's side way goes on: the road
                // met the cross road square, the way straight on was the cross road's far kerb (a section's side
                // piece against a wall), the walk ended on it and the car rolled into the wall
                auto leads_on = [&](const Branch& b)
                {
                    if (b.seg < 0 || b.seg >= S32(sParts.size()))
                    {
                        return true;
                    }
                    const PieceShapes& pb = *sParts[b.seg];
                    F64 ax, ay, along, across;
                    partAxis(pb, b.dx, b.dy, ax, ay, along, across);
                    if (along >= LONE_PIECE_M)
                    {
                        return true;
                    }
                    for (const Continuation& c : sConts[b.seg])
                    {
                        if (c.other != cur && visited[c.other] != walk && (sPartDefinite[c.other] || sPartPending[c.other])
                            && (c.x - b.x) * b.dx + (c.y - b.y) * b.dy > 0.5)
                        {
                            return true;
                        }
                    }
                    return false;
                };
                const bool side_on = std::any_of(branches.begin(), branches.end(),
                                                 [&](const Branch& b) { return b.side && std::fabs(b.turn_deg) >= 30.0 && leads_on(b); });
                auto goes_on = [&](const Branch& b) { return (std::fabs(b.turn_deg) < 30.0 && !side_on) || leads_on(b); };
                std::vector<Branch> kept;
                for (const Branch& b : branches)
                {
                    if (goes_on(b))
                    {
                        kept.push_back(b);
                    }
                }
                if (!kept.empty())
                {
                    branches.swap(kept);
                }
            }
            if (branches.size() > 1 && !host::privateRoadsAllowed())
            {
                // a choice is made only onto land known to be Linden road: one whose owner is not in yet is not offered
                std::vector<Branch> kept;
                for (const Branch& br : branches)
                {
                    if (!br.land_unknown)
                    {
                        kept.push_back(br);
                    }
                    else
                    {
                        logLand(br.id(), "not offered at a junction: the land's owner not known yet");
                    }
                }
                if (!kept.empty())
                {
                    branches.swap(kept);
                }
            }
            if (branches.size() > 1 && sPartDefinite[cur])
            {
                // a turn off a road is offered only onto road: a piece only road-shaped (a car park, a driveway, a
                // yard's paving) is no choice, though explore weighed its being undriven over the road going on
                std::vector<Branch> kept;
                for (const Branch& b : branches)
                {
                    if (std::fabs(b.turn_deg) < 30.0 || b.definite)
                    {
                        kept.push_back(b);
                    }
                    else
                    {
                        LL_DEBUGS("SKRoadDriver") << "junction: " << b.kind() << " onto " << b.root << " not offered (not road outright)" << LL_ENDL;
                    }
                }
                if (!kept.empty())
                {
                    branches.swap(kept);
                }
            }
            if (branches.size() > 1)
            {
                auto ahead = [&](const Branch& b)
                {
                    if (b.seg < 0 || b.seg >= S32(sParts.size()))
                    {
                        return false;
                    }
                    const PieceShapes& pb = *sParts[b.seg];
                    const F64 vx = pb.cx - px, vy = pb.cy - py;
                    const F64 along = vx * dx + vy * dy, across = -vx * dy + vy * dx;
                    return along > 0 && std::atan2(std::fabs(across), along) * 180 / PI < AHEAD_DEG;
                };
                S32 straightest = -1;
                for (S32 i = 0; i < S32(branches.size()); ++i)
                {
                    if (ahead(branches[i]) && (straightest < 0 || std::fabs(branches[i].turn_deg) < std::fabs(branches[straightest].turn_deg)))
                    {
                        straightest = i;
                    }
                }
                if (straightest >= 0)
                {
                    std::vector<Branch> kept;
                    for (S32 i = 0; i < S32(branches.size()); ++i)
                    {
                        if (i == straightest || !ahead(branches[i]))
                        {
                            kept.push_back(branches[i]);
                        }
                    }
                    branches.swap(kept);
                }
            }
            // Overlapping pieces can expose both the next piece and its successor as exits.
            if (branches.size() > 1)
            {
                std::vector<Branch> kept;
                for (const Branch& b : branches)
                {
                    bool successor = false;
                    for (const Branch& o : branches)
                    {
                        if (o.seg == b.seg || o.seg < 0 || b.seg < 0 || o.dx * b.dx + o.dy * b.dy <= 0)
                            continue;
                        for (const Continuation& c : sConts[o.seg])
                        {
                            if (c.other == b.seg && (c.x - o.x) * o.dx + (c.y - o.y) * o.dy > 0.5
                                && (c.x - b.x) * b.dx + (c.y - b.y) * b.dy < 0)
                                successor = true;
                        }
                    }
                    if (!successor)
                        kept.push_back(b);
                }
                if (!kept.empty())
                    branches.swap(kept);
            }
            if (branches.empty())
            {
                // this half meets nothing on, while the road's other half alongside does: on a bend the next
                // linkset's piece lies ahead of the other lane and meets this half only at a corner, no join (the
                // walk stopped under the Montdidier arch and the line stalled): on through the other half's way on,
                // the straightest
                for (S32 half : halves)
                {
                    const PieceShapes& ph = *sParts[half];
                    for (const Continuation& c2 : sConts[half])
                    {
                        if (c2.other == cur || visited[c2.other] == walk || (!sPartDefinite[c2.other] && !sPartPending[c2.other])
                            || (c2.x - px) * dx + (c2.y - py) * dy < 0.5 || (c2.x - ph.cx) * dx + (c2.y - ph.cy) * dy <= 0)
                        {
                            continue;
                        }
                        const PieceShapes& pn = *sParts[c2.other];
                        if (on_surface && !ofSurface(pn) && !leadsToSurface(half, c2.other, c2.x, c2.y, dx, dy))
                        {
                            continue;
                        }
                        const TopEdge& eb = pn.top[c2.other_edge];
                        F64 bdx = -eb.ox, bdy = -eb.oy;
                        {
                            // met through the other half, the piece on may be entered by its side (a deck in two
                            // halves, offset from the ramp's: the walk turned 90 deg across the Agirus bridge and
                            // stopped); where the piece runs on our way, in line with the walk (its middle within
                            // half its width of the way: not a cross street's piece off to the side), it is gone
                            // along, not across
                            F64 ax, ay, along, across;
                            partAxis(pn, dx, dy, ax, ay, along, across);
                            const F64 aside = std::fabs(-(pn.cx - px) * dy + (pn.cy - py) * dx);
                            if (bdx * dx + bdy * dy < std::cos(SIDE_ENTRY_DEG * PI / 180) && ax * dx + ay * dy > std::cos(ALONG_ENTRY_DEG * PI / 180)
                                && aside < across / 2)
                            {
                                bdx = ax;
                                bdy = ay;
                            }
                        }
                        bool land_unknown = false;
                        if (landRefuses(cur, c2.other, c2.x, c2.y, bdx, bdy, land_unknown))
                        {
                            ++why_land;
                            continue;
                        }
                        F64 bax, bay, balong, bacross;
                        partAxis(pn, bdx, bdy, bax, bay, balong, bacross);
                        const F64 turn = std::atan2(dx * bdy - dy * bdx, bdx * dx + bdy * dy) * 180 / PI;
                        if (bacross < MIN_BRANCH_W || std::fabs(turn) > MAX_BRANCH_TURN_DEG
                            || (bacross < turnMinBranchW() && std::fabs(turn) >= TURN_NARROW_DEG)
                            || (!branches.empty() && std::fabs(turn) >= std::fabs(branches[0].turn_deg)))
                        {
                            continue;
                        }
                        Branch b;
                        b.seg = c2.other;
                        b.root = pn.root;
                        b.part = pn.part;
                        b.x = c2.x;
                        b.y = c2.y;
                        b.dx = bdx;
                        b.dy = bdy;
                        b.same_surface = !pc.top_known || !pn.top_known || pc.top_tex == pn.top_tex;
                        b.definite = sPartDefinite[c2.other];
                        b.on_route = sRoute.count(pn.root) > 0;
                        b.turn_deg = turn;
                        branches.assign(1, b);
                    }
                }
            }
            if (branches.empty() && !sView.jump)
            {
                // a gap with the road going on beyond: a jump, the line drawn across it, along the way the line
                // came up the ramp (a short mesh ramp's own axis was 25 degrees off the road's)
                F64 jdx = dx, jdy = dy, runx = px - JUMP_RUNUP_M * dx, runy = py - JUMP_RUNUP_M * dy;
                for (size_t k = line.size(); k-- > 0;)
                {
                    const F64 bx = px - line[k].x, by = py - line[k].y, bl = std::hypot(bx, by);
                    if (bl >= JUMP_RUNUP_M)
                    {
                        if (bx * dx + by * dy > 0.7 * bl)
                        {
                            jdx = bx / bl;
                            jdy = by / bl;
                            runx = line[k].x;
                            runy = line[k].y;
                        }
                        break;
                    }
                }
                const F64 through = farSide(pc, px, py, jdx, jdy);
                const F64 lipx = px + through * jdx, lipy = py + through * jdy;
                F64 lipz;
                F64 lx, ly, lz;
                // a ramp: the lip above the run-up to it (a seam between level pieces is no jump)
                const S32 land = partHeightAt(pc, lipx - 0.5 * jdx, lipy - 0.5 * jdy, lipz) && lipz - surfaceAt(runx, runy, lipz) >= JUMP_RAMP_M
                                     ? jumpLanding(cur, visited, walk, lipx, lipy, lipz, jdx, jdy, lx, ly, lz) : -1;
                bool land_unknown = false;
                if (land >= 0 && landRefuses(cur, land, lx, ly, jdx, jdy, land_unknown))
                {
                    ++why_land;
                }
                else if (land >= 0 && std::hypot(lx - lipx, ly - lipy) >= JUMP_GAP_M)
                {
                    sView.jump = true;
                    jump_step = walked.size() - 1;
                    sView.jump_x = lipx;
                    sView.jump_y = lipy;
                    sView.jump_z = lipz;
                    sView.land_x = lx;
                    sView.land_y = ly;
                    sView.land_z = lz;
                    sView.jump_dx = jdx;
                    sView.jump_dy = jdy;
                    line.push_back({ lipx, lipy, cur, cur });
                    line.push_back({ lx, ly, land, land });
                    travelled += through + std::hypot(lx - lipx, ly - lipy);
                    visited[land] = walk;
                    sWalkParts.push_back(land);
                    if (sChainRoots.back() != sParts[land]->root)
                    {
                        sChainRoots.push_back(sParts[land]->root);
                    }
                    cur = land;
                    px = lx;
                    py = ly;
                    RouteStep st;
                    st.part = sParts[land];
                    st.id = sParts[land]->part;
                    st.x = lx;
                    st.y = ly;
                    st.jump = true;
                    walked.push_back(std::move(st));
                    walked_line.push_back(line.size() - 1);
                    if (follow >= 0 && follow + 1 < S32(sCommitted.size()) && sCommitted[follow + 1].part == sParts[land])
                    {
                        ++follow;
                        ++followed;
                    }
                    else
                    {
                        stopFollowing("the gap lands elsewhere");
                    }
                    F64 ax, ay, along, across;
                    partAxis(*sParts[land], jdx, jdy, ax, ay, along, across);
                    dx = partSquare(*sParts[land]) ? jdx : ax;
                    dy = partSquare(*sParts[land]) ? jdy : ay;
                    walked.back().dx = dx;
                    walked.back().dy = dy;
                    continue;
                }
            }
            if (branches.empty())
            {
                // why it ends here, for the view log: the part, where, and what its meetings were turned away for
                // the road's end: ground was all there was ahead (see leadsToSurface), beside the road's own other
                // half and strips (a way too sharp could be the road doubling back: no end)
                // or a way on was refused (a pavement, an open area, a destination, land not Linden's)
                // or nothing to walk at all ahead (no meeting but ground that is not road): a road end, unless the
                // part's far side is at a region edge, where the next region's pieces may not be in yet
                const F64 far_m = farSide(pc, px, py, dx, dy);
                const F64 ex = std::fmod(px + far_m * dx, 256.0), ey = std::fmod(py + far_m * dy, 256.0);
                const bool at_edge = ex < 8.0 || ex > 248.0 || ey < 8.0 || ey > 248.0;
                const bool nothing = why_visited == 0 && why_behind == 0 && why_beside == 0 && why_strip == 0 && why_turn == 0
                                     && why_ground == 0 && why_land == 0;
                const bool at_end = (why_ground > 0 && why_turn == 0) || why_land > 0 || (nothing && !at_edge);
                if (why_land > 0 || nothing)
                {
                    logLand(pc.part, why_land > 0 ? "line ends: every way on refused (road end)"
                                     : at_edge  ? "line ends at a region edge: nothing to walk yet (waiting for its pieces)"
                                                : "line ends: nothing to walk ahead (road end)");
                }
                stuck = !at_end && at >= 0;
                sView.walk_end = llformat("%spart %s at %.1f,%.1f after %.0f m: meetings %d (visited %d, not road %d, behind %d, beside %d, "
                                          "strip %d, too sharp %d, ground %d, refused land %d)", at_end ? WALK_END_ROAD_END : "",
                                          pc.part.asString().substr(0, 8).c_str(), px, py, travelled, S32(sConts[cur].size()), why_visited,
                                          why_not_road, why_behind, why_beside, why_strip, why_turn, why_ground, why_land);
                // the road ends here (or its next part has not come yet): on to the part's far side
                const F64 through = farSide(pc, px, py, dx, dy);
                line.push_back({ px + through * dx, py + through * dy, cur, cur });
                if (at_end)
                {
                    sView.road_end = true;
                    sView.road_end_x = px + through * dx;
                    sView.road_end_y = py + through * dy;
                    sView.road_end_dx = dx;
                    sView.road_end_dy = dy;
                }
                ended = true;
                break;
            }
            S32 pick = -1;
            if (follow >= 0 && follow + 1 < S32(sCommitted.size()))
            {
                // the choice made before stands while its way is still one of the ways on
                const RouteStep& nx = sCommitted[follow + 1];
                for (S32 i = 0; i < S32(branches.size()) && pick < 0; ++i)
                {
                    if (branches[i].seg >= 0 && sParts[branches[i].seg] == nx.part && branches[i].part == nx.id && !nx.jump)
                    {
                        pick = i;
                    }
                }
                if (pick >= 0)
                {
                    ++follow;
                    ++followed;
                }
                else
                {
                    stopFollowing(nx.jump ? "the gap not found again" : "not a way on from its junction now");
                }
            }
            Junction j;
            if (branches.size() > 1)
            {
                j.from = pc.part;
                j.x = px;
                j.y = py;
                j.branches = branches;
                const bool near_commit = sCommit.valid && std::hypot(px - sCommit.x, py - sCommit.y) < COMMIT_NEAR_M;
                const S32 held = near_commit ? committedBranch(branches) : -1;
                if (held >= 0)
                {
                    pick = held;    // the choice made at this junction holds until it is passed, over the route's
                }
                else if (near_commit)
                {
                    LL_INFOS("SKRoadDriver") << "junction at " << S32(sCommit.x) << ", " << S32(sCommit.y) << ": committed way ("
                                             << S32(sCommit.turn_deg) << " deg) is no longer a way on: choosing again" << LL_ENDL;
                }
                j.chosen = pick >= 0 ? pick : chooseBranch(j);
                pick = j.chosen;
                if (held < 0 && (!sCommit.valid || near_commit))
                {
                    const Branch& c = branches[pick];
                    sCommit.valid = true;
                    sCommit.x = px;
                    sCommit.y = py;
                    sCommit.dx = c.dx;
                    sCommit.dy = c.dy;
                    sCommit.turn_deg = c.turn_deg;
                    sCommit.id = c.id();
                    sCommit.root = c.root;
                }
                sView.junctions.push_back(j);
                met_junctions.push_back({ walked.size() - 1, j });
            }
            advance(branches[std::max(pick, 0)], branches.size() > 1);
        }
        S32 keep_from = -1;     // the committed steps past the walk's reach, kept as they were
        if (follow >= 0 && follow + 1 < S32(sCommitted.size()))
        {
            keep_from = follow + 1;
        }
        if (at >= 0)
        {
            sView.route = from_car ? llformat("followed %d of %d ahead", followed, S32(sCommitted.size()) - 1 - at)
                                   : llformat("stored, walked on from %.0f m ahead: followed %d of %d", s0 - s_car, followed,
                                              S32(sCommitted.size()) - 1 - resume);
            if (!store_why.empty())
            {
                sView.route += ", re-laid for " + store_why;
            }
            if (!broke.empty())
            {
                sView.route += ", " + broke;
                LL_INFOS("SKRoadDriver") << "road route " << sView.route << LL_ENDL;
            }
        }
        // the route kept: the way travelled behind the car (as far back as ROUTE_HISTORY_M), the route walked from
        // the car on, and what was committed past the walk's reach
        std::vector<RouteStep> walked_all = walked;
        S32 car_at = 0;     // the car's step in the route kept
        {
            std::deque<RouteStep> route;
            if (at > 0)
            {
                S32 from = at;
                for (F64 back = 0; from > 0 && back < ROUTE_HISTORY_M; --from)
                {
                    back += std::hypot(sCommitted[from].x - sCommitted[from - 1].x, sCommitted[from].y - sCommitted[from - 1].y);
                }
                route.assign(sCommitted.begin() + from, sCommitted.begin() + at);
            }
            car_at = S32(route.size());
            walked_n = walked.size();
            route.insert(route.end(), std::make_move_iterator(walked.begin()), std::make_move_iterator(walked.end()));
            if (keep_from >= 0)
            {
                route.insert(route.end(), sCommitted.begin() + keep_from, sCommitted.end());
            }
            sCommitted.swap(route);
        }
        sLastTravelled = travelled;
        WalkOut mine_out;
        mine_out.line = std::move(line);
        mine_out.base = line_base;
        mine_out.walked = std::move(walked_all);
        mine_out.walked_line = std::move(walked_line);
        mine_out.prefilled = prefilled;
        mine_out.junctions = std::move(met_junctions);
        mine_out.surface_changes = std::move(met_surface);
        mine_out.signs = std::move(met_signs);
        mine_out.jump_step = jump_step;
        mine_out.ended = ended;
        mine_out.stuck = stuck;
        mine_out.dx = dx;
        mine_out.dy = dy;
        std::vector<LanePoint> out;
        if (!storing)
        {
            out = partLane(mine_out.line, x, y, z, dx, dy, keep);
        }
        if (stuck && sRepairDepth == 0)
        {
            // the route ends short with the road not ending there (its far end met nothing more): a walk from the car
            // afresh may see further (the choices at junctions still kept, chooseBranch's), and where it reaches
            // ROUTE_FRESH_M further it is the route from the car on, repaired; the way travelled stays
            const std::deque<RouteStep> mine = sCommitted;
            const size_t history = sCommitted.size() - walked_n;
            const View view = sView;
            const std::vector<S32> parts = sWalkParts;
            const std::vector<LLUUID> roots = sChainRoots;
            const F64 off_since = sOffRouteSince;
            sCommitted.clear();
            sView.junctions.clear();
            sView.surface_changes.clear();
            sView.walk_end.clear();
            sView.road_end = false;
            sView.jump = false;
            sView.sign_speed_limit = 0.f;
            sView.sign_warning = 0;
            ++sRepairDepth;
            std::vector<LanePoint> fresh = boxWalk(x, y, z, hx, hy, v, keep);
            --sRepairDepth;
            // ... and does not run back over the way travelled (a walk afresh off a part entered askew ran 100 m back
            // the way the car came)
            auto forward = [&](const std::vector<LanePoint>&)
            {
                for (size_t i = 1; i < sCommitted.size(); ++i)
                {
                    for (size_t h = 0; h + 1 < history; ++h)
                    {
                        if (sCommitted[i].part == mine[h].part)
                        {
                            return false;
                        }
                    }
                }
                // ... and, past the first stretch, carries the route on rather than taking another way to the same
                // place (a far end that changed its way blipped the line)
                if (travelled < ROUTE_FRESH_KEEP_M)
                {
                    return true;
                }
                // a route that ended by a region's edge (its last step entered within EDGE_HOOK_M of it) ended where the next region's pieces had not come: its last
                // step is wherever the walk turned off to there (the kerb strip), no place for a walk afresh to
                // reach, and keeping it held the hook for the whole run up to the edge
                if (toRegionEdge(mine.back().x, mine.back().y) <= EDGE_HOOK_M)
                {
                    return true;
                }
                for (size_t i = 1; i < sCommitted.size(); ++i)
                {
                    if (sCommitted[i].part == mine.back().part)
                    {
                        return true;
                    }
                }
                return false;
            };
            if (!fresh.empty() && sLastTravelled > travelled + ROUTE_FRESH_M && forward(fresh))
            {
                sCommitted.insert(sCommitted.begin(), mine.begin(), mine.begin() + history);
                sView.route = llformat("repaired: walked afresh from the car, %.0f m further", sLastTravelled - travelled);
                if (!storing)
                {
                    return fresh;
                }
                // the walk afresh is the stored road from the car on
                WalkOut fresh_out = std::move(sWalkOut);
                if (k_at >= 0 && !sStored.empty() && !fresh_out.walked.empty() && fresh_out.walked[0].id == sStored.steps[k_at].part)
                {
                    storeTruncate(size_t(k_at), sStored.car_s);
                    storeLay(fresh_out, size_t(k_at), sStored.car_s, true, x, y, z, keep);
                }
                else
                {
                    sStored.clear();
                    storeLay(fresh_out, 0, 0.0, true, x, y, z, keep);
                }
                std::string none;
                storeSeen(false, none);
                return storeOut(x, y, std::min<S32>(S32(history), S32(sCommitted.size()) - 1), length);
            }
            sCommitted = mine;
            sView = view;
            sWalkParts = parts;
            sChainRoots = roots;
            sOffRouteSince = off_since;
            sLastTravelled = travelled;
        }
        if (!storing)
        {
            sWalkOut = std::move(mine_out);
            return out;
        }
        storeLay(mine_out, k_at >= 0 ? size_t(k_at) : 0, s0, from_car, x, y, z, keep);
        std::string none;
        storeSeen(false, none);
        if (sStored.stuck && !sStored.steps.empty())
        {
            // walked again at a stuck end only once something could have changed
            sStored.stuck_parts_gen = sPartsGen;
            sStored.stuck_verdict_gen = sVerdictGen;
            sStored.stuck_car_step = sStored.steps[std::max(storeStep(sCommitted[car_at].id), 0)].serial;
        }
        return storeOut(x, y, car_at, length);
    }

    // The lane line ahead from the segments: chained end to end. Where more than one way leaves a segment
    // it is a junction, and chooseBranch picks the way on.
    std::vector<LanePoint> chain(F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 v, F64 keep)
    {
        std::vector<LanePoint> out;
        S32 cur = segmentUnder(x, y, z);
        const bool route_fresh = !sRoute.empty() && sClock.getElapsedTimeF64() - sRouteTime < ROUTE_KEEP_S;
        if (route_fresh && (cur < 0 || !sRoute.count(sSegments[cur].root)))
        {
            // the car is on no piece, or on one off the route (a cover, a seam): the route's piece nearest it
            S32 best = -1;
            F64 best_d = ROUTE_NEAR_M;
            for (const LLUUID& root : sRoute)
            {
                auto f = sSegByRoot.find(root);
                if (f == sSegByRoot.end())
                {
                    continue;
                }
                const Segment& g = sSegments[f->second];
                if (std::fabs(zAt(g, x, y) - z) > Z_REACH)
                {
                    continue;
                }
                const F64 a = std::clamp((x - g.cx) * g.ax + (y - g.cy) * g.ay, g.lo, g.hi);
                const F64 d = std::max(0.0, std::hypot(x - (g.cx + a * g.ax), y - (g.cy + a * g.ay)) - g.width / 2);
                if (d < best_d)
                {
                    best_d = d;
                    best = f->second;
                }
            }
            if (best >= 0)
            {
                cur = best;
            }
        }
        if (cur < 0 || !sSegments[cur].definite)
        {
            // on no road piece at all (drifted onto the verge, or stopped beside the road), or on one only
            // road-shaped (a private pavement beside it): the nearest definite road piece within reach, along
            // the way the car faces, so it finds its way back; any road piece where there is none
            const S32 under = cur;
            cur = -1;
            for (S32 pass = 0; pass < (under < 0 ? 2 : 1) && cur < 0; ++pass)
            {
                F64 best_d = ROUTE_NEAR_M;
                for (S32 k = 0; k < S32(sSegments.size()); ++k)
                {
                    const Segment& g = sSegments[k];
                    if (g.width < MIN_BRANCH_W || std::fabs(zAt(g, x, y) - z) > Z_REACH || (!g.definite && pass == 0)
                        || (!g.square && std::fabs(g.ax * hx + g.ay * hy) < 0.7))
                    {
                        continue;
                    }
                    const F64 a = std::clamp((x - g.cx) * g.ax + (y - g.cy) * g.ay, g.lo, g.hi);
                    const F64 d = std::max(0.0, std::hypot(x - (g.cx + a * g.ax), y - (g.cy + a * g.ay)) - g.width / 2);
                    if (d < best_d)
                    {
                        best_d = d;
                        cur = k;
                    }
                }
            }
            if (cur < 0)
            {
                cur = under;
            }
        }
        if (cur < 0)
        {
            return out;
        }
        const F64 length = lookAhead(v);
        std::unordered_set<S32> visited{ cur };
        // the surface the road ahead is in, carried along the chain: the car's, until a linkset in another
        std::unordered_set<LLUUID> surface = sSegments[cur].surface;
        sChainRoots.assign(1, sSegments[cur].root);
        F64 dx = hx, dy = hy;                   // the direction of travel along the chain
        F64 px = x, py = y;                     // where on the current segment the line enters
        F64 travelled = 0;
        F64 last_shift = 0;
        bool have_shift = false;
        while (travelled < length)
        {
            const Segment& sg = sSegments[cur];
            // entered across its long side (a crossing laid over the road, wider than it is deep): a slab to
            // drive across, as a square one, not a road going sideways
            const bool slab = sg.square || std::fabs(sg.ax * dx + sg.ay * dy) < 0.5;
            F64 sax = sg.ax, say = sg.ay;
            F64 s_in, s_out;
            F64 ex, ey;
            if (!slab)
            {
                if (sax * dx + say * dy < 0)
                {
                    sax = -sax;
                    say = -say;
                }
                // along its axis from where the line enters to its far end
                s_in = (px - sg.cx) * sax + (py - sg.cy) * say;
                s_out = (sax == sg.ax) ? sg.hi : -sg.lo;
                const S32 lanes = sg.width >= TWO_LANES_M ? 2 : 1;
                const F64 shift = lanes == 2 ? laneShiftIn(sg.width, keep) : 0.0;
                const F64 rx = say, ry = -sax;
                for (F64 s_ = std::max(s_in, sax == sg.ax ? sg.lo : -sg.hi); s_ <= s_out + 1e-6; s_ += PATH_STEP)
                {
                    LanePoint lp;
                    lp.x = sg.cx + s_ * sax + shift * rx;
                    lp.y = sg.cy + s_ * say + shift * ry;
                    lp.ax = sax;
                    lp.ay = say;
                    lp.lane_w = sg.width / lanes;
                    lp.road_w = sg.width;
                    lp.lanes = lanes;
                    lp.z = zAt(sg, lp.x, lp.y);
                    lp.left_x = sg.cx + s_ * sax - sg.width / 2 * rx;
                    lp.left_y = sg.cy + s_ * say - sg.width / 2 * ry;
                    lp.right_x = sg.cx + s_ * sax + sg.width / 2 * rx;
                    lp.right_y = sg.cy + s_ * say + sg.width / 2 * ry;
                    out.push_back(lp);
                    travelled += PATH_STEP;
                }
                last_shift = shift;
                have_shift = true;
                dx = sax;
                dy = say;
                ex = sg.cx + s_out * sax;
                ey = sg.cy + s_out * say;
            }
            else
            {
                // no direction of its own: its ways on are looked for around all of it (below), and the line
                // drawn across it once one is chosen
                ex = px;
                ey = py;
            }

            // every way on: a segment's end near this one's end (around a square one: near its edge), leaving
            // no sharper than back the way we came
            std::vector<Branch> ways;
            // The pieces this one meets along its top's edges, ahead of where the line came in: at its far end the
            // road going on, along its sides a road joining (a junction). A piece alongside, parallel and joined
            // along a side edge, is the road's other half: not a way on.
            for (const Join& jn : sJoins[cur])
            {
                if (visited.count(jn.other) || jn.length < JOIN_MIN_WAY_M)
                {
                    continue;
                }
                const Segment& nx = sSegments[jn.other];
                if (((jn.x - px) * dx + (jn.y - py) * dy) < 0.5)
                {
                    continue;       // behind where the line came in, or beside it (a slab's too: a road of short
                                    // unlinked pieces is a run of slabs, each meeting its neighbours all round)
                }
                const bool side_edge = !slab && std::fabs(jn.ex * sg.ax + jn.ey * sg.ay) > 0.9;
                if (side_edge && !nx.square && std::fabs(nx.ax * sg.ax + nx.ay * sg.ay) > std::cos(SIBLING_DEG * PI / 180))
                {
                    continue;       // the other half alongside
                }
                Branch b;
                b.seg = jn.other;
                b.root = nx.root;
                b.on_route = sRoute.count(nx.root) > 0;
                b.same_surface = nx.matches;
                b.definite = nx.definite;
                b.x = jn.x;
                b.y = jn.y;
                if (nx.square || std::fabs(nx.ax * dx + nx.ay * dy) < 0.5)
                {
                    slabWay(sg, slab, ex, ey, nx, dx, dy, b.dx, b.dy);
                }
                else
                {
                    // its axis, away from the join
                    const bool away = (nx.cx - jn.x) * nx.ax + (nx.cy - jn.y) * nx.ay >= 0;
                    b.dx = away ? nx.ax : -nx.ax;
                    b.dy = away ? nx.ay : -nx.ay;
                }
                const F64 c = b.dx * dx + b.dy * dy, sn = dx * b.dy - dy * b.dx;
                b.turn_deg = std::atan2(sn, c) * 180 / PI;
                if (std::fabs(b.turn_deg) > MAX_BRANCH_TURN_DEG)
                {
                    continue;
                }
                ways.push_back(b);
            }
            const F64 sq_r = slab ? squareRadius(sg) : 0.0;
            const F64 cx0 = slab ? sg.cx : ex, cy0 = slab ? sg.cy : ey;
            const F64 r0 = slab ? sq_r + JOIN_M : JOIN_M + 8.0;
            // Where this piece meets others ahead along its top's edges, those joins are its ways on and no
            // others: a pavement lying under the road's edge shares no top edge with it, however near its end
            // lies. The ends-and-reach search below is for where no join leads on (a cover laid over the road
            // meets it on no edge), and never takes a piece lower than this one where they meet.
            const bool joined = !ways.empty();
            for (S64 gx = S64(std::floor((cx0 - r0) / HASH_M)); !joined && gx <= S64(std::floor((cx0 + r0) / HASH_M)); ++gx)
            {
                for (S64 gy = S64(std::floor((cy0 - r0) / HASH_M)); gy <= S64(std::floor((cy0 + r0) / HASH_M)); ++gy)
                {
                    auto it = sSegEnds.find(hashKey(gx, gy));
                    if (it == sSegEnds.end())
                    {
                        continue;
                    }
                    for (S32 j : it->second)
                    {
                        if (visited.count(j) || std::any_of(ways.begin(), ways.end(), [j](const Branch& b) { return b.seg == j; }))
                        {
                            continue;
                        }
                        const Segment& nx = sSegments[j];
                        if (nx.width < MIN_BRANCH_W && siblings(j).empty())
                        {
                            continue;       // not a road to drive on: a sign, a post
                        }
                        Branch b;
                        b.seg = j;
                        b.root = nx.root;
                        b.on_route = sRoute.count(nx.root) > 0;
                        b.same_surface = nx.matches;
                        b.definite = nx.definite;
                        if (nx.square || std::fabs(nx.ax * dx + nx.ay * dy) < 0.5)
                        {
                            // entered along its edge in front of us, on into it the way we were going; from a
                            // slab (a road of short, wide pieces), where the line leaves it straight on
                            F64 qx = ex, qy = ey;
                            if (slab)
                            {
                                const F64 through = reach(sg, px, py, dx, dy);
                                qx = px + through * dx;
                                qy = py + through * dy;
                            }
                            const F64 along = (nx.cx - qx) * dx + (nx.cy - qy) * dy;
                            const F64 side = std::fabs(-(nx.cx - qx) * dy + (nx.cy - qy) * dx);
                            if (!(along > 0 && along < nx.width && side < nx.width / 2 + JOIN_M))
                            {
                                continue;
                            }
                            slabWay(sg, slab, ex, ey, nx, dx, dy, b.dx, b.dy);
                            b.x = qx;
                            b.y = qy;
                        }
                        else
                        {
                            bool found = false;
                            F64 best_d = 1e9;
                            for (F64 e : { nx.lo, nx.hi })
                            {
                                const F64 nex = nx.cx + e * nx.ax, ney = nx.cy + e * nx.ay;
                                const F64 ndx = e == nx.lo ? nx.ax : -nx.ax, ndy = e == nx.lo ? nx.ay : -nx.ay;
                                F64 d;
                                if (slab)
                                {
                                    // its end at the slab's edge, leaving outward
                                    d = std::hypot(nex - sg.cx, ney - sg.cy) - sq_r;
                                    if (d > JOIN_M || (nex - sg.cx) * ndx + (ney - sg.cy) * ndy < 0)
                                    {
                                        continue;
                                    }
                                }
                                else
                                {
                                    d = std::hypot(nex - ex, ney - ey);
                                    if (d > std::max(JOIN_M, nx.width / 2))
                                    {
                                        continue;
                                    }
                                }
                                if (d < best_d)
                                {
                                    best_d = d;
                                    found = true;
                                    b.dx = ndx;
                                    b.dy = ndy;
                                    b.x = nex;
                                    b.y = ney;
                                }
                            }
                            if (!found)
                            {
                                continue;
                            }
                        }
                        if (std::fabs(zAt(nx, b.x, b.y) - zAt(sg, b.x, b.y)) > MAX_BRANCH_DZ)
                        {
                            continue;       // where they meet, one is above the other: not joined
                        }
                        if (zAt(nx, b.x, b.y) < zAt(sg, b.x, b.y) - UNDERLAP_M)
                        {
                            continue;       // lower where they meet: a pavement under the road's edge, not the road on
                        }
                        const F64 c = b.dx * dx + b.dy * dy, sn = dx * b.dy - dy * b.dx;
                        b.turn_deg = std::atan2(sn, c) * 180 / PI;
                        if (std::fabs(b.turn_deg) > MAX_BRANCH_TURN_DEG)
                        {
                            continue;       // back the way we came
                        }
                        ways.push_back(b);
                    }
                }
            }
            // ways on in nearly the same direction are one branch (a road split into parallel strips): the
            // better of them stands for it
            std::sort(ways.begin(), ways.end(), [](const Branch& a, const Branch& b) { return branchScore(a) > branchScore(b); });
            // Within a branch the way nearest the lane line the car keeps stands for it: a road built as two
            // linksets side by side, one a half, carries on as two ways straight on, and the half the car is
            // in is the one to follow (scores alike, the other half was taken and the line crossed over).
            const F64 lx = out.empty() ? px : out.back().x, ly = out.empty() ? py : out.back().y;
            auto aside = [&](const Branch& b) { return std::fabs(-(b.x - lx) * dy + (b.y - ly) * dx); };
            std::vector<Branch> branches;
            for (const Branch& b : ways)
            {
                auto same = std::find_if(branches.begin(), branches.end(),
                                         [&b](const Branch& o) { return std::fabs(o.turn_deg - b.turn_deg) < SAME_BRANCH_DEG; });
                if (same == branches.end())
                {
                    branches.push_back(b);
                }
                else if ((b.definite && !same->definite)
                         || (b.definite == same->definite && b.same_surface == same->same_surface && aside(b) < aside(*same)))
                {
                    *same = b;
                }
            }
            if (branches.empty())
            {
                if (slab)
                {
                    const F64 through = reach(sg, px, py, dx, dy);
                    drawAcross(out, travelled, sg, px, py, dx, dy, px + through * dx, py + through * dy, dx, dy,
                               have_shift ? last_shift : 0.0);
                }
                break;      // the road ends: the chain stops here
            }
            S32 pick = 0;
            if (branches.size() > 1)
            {
                Junction jn;
                jn.from = sg.root;
                jn.x = slab ? sg.cx : ex;
                jn.y = slab ? sg.cy : ey;
                jn.branches = branches;
                pick = chooseBranch(jn);
                jn.chosen = pick;
                sView.junctions.push_back(std::move(jn));
            }
            else if (std::fabs(branches[0].turn_deg) > MAX_JOIN_TURN_DEG && !slab)
            {
                break;      // the only way on turns sharper than a continuation: the chain stops here
            }
            const Branch go = branches[pick];
            if (slab)
            {
                drawAcross(out, travelled, sg, px, py, dx, dy, go.x, go.y, go.dx, go.dy, have_shift ? last_shift : 0.0);
            }
            {
                // Stepping onto a linkset whose textures are none of the surface's: the road's surface ends here.
                // A junction or crossing slab between does not change it, nor a linkset whose textures are not
                // known yet. The road's shape going on in line and about as wide, both measured across the whole
                // road (its halves alongside too), it is the same road re-surfaced (asphalt into cobbles);
                // otherwise another road starts here.
                const Segment& to = sSegments[go.seg];
                if (!to.square && !to.surface.empty() && !surface.empty() && !sameKit(sChainRoots.back(), go.root))
                {
                    // under half their textures in common: asphalt {asphalt, kerb} and cobbles {cobbles, kerb}
                    // share a third; paving variants of one road share all
                    size_t common = 0;
                    for (const LLUUID& t : to.surface)
                    {
                        common += surface.count(t);
                    }
                    const size_t either = surface.size() + to.surface.size() - common;
                    if (2 * common < either)
                    {
                        F64 a_lo, a_hi, b_lo, b_hi;
                        const F64 bx = go.x - go.dx * 1.0, by = go.y - go.dy * 1.0;   // just before, on this one
                        roadAcross(cur, bx, by, go.dx, go.dy, a_lo, a_hi);
                        roadAcross(go.seg, go.x, go.y, go.dx, go.dy, b_lo, b_hi);
                        const F64 ratio = (b_hi - b_lo) / std::max(0.1, a_hi - a_lo);
                        const bool in_line = std::fabs((a_lo + a_hi) / 2 - (b_lo + b_hi) / 2) < 1.5;
                        const bool same_road = (slab || std::fabs(go.turn_deg) < 15) && in_line
                                               && ratio > 1 / SAME_ROAD_WIDTH && ratio < SAME_ROAD_WIDTH;
                        const F64 rx = go.dy, ry = -go.dx, mid = (b_lo + b_hi) / 2;
                        sView.surface_changes.push_back({ go.x + mid * rx, go.y + mid * ry, zAt(to, go.x, go.y), go.dx, go.dy,
                                                          b_hi - b_lo, same_road });
                        surface = to.surface;
                    }
                }
                else if (surface.empty() && !to.square)
                {
                    surface = to.surface;
                }
            }
            visited.insert(go.seg);
            sChainRoots.push_back(go.root);
            cur = go.seg;
            px = go.x;
            py = go.y;
            dx = go.dx;
            dy = go.dy;
        }
        // continuity across segments of different widths: at most MAX_SHIFT_M aside per step
        for (size_t i = 1; i < out.size(); ++i)
        {
            const LanePoint& a = out[i - 1];
            LanePoint& b = out[i];
            const F64 rx = a.ay, ry = -a.ax;
            const F64 side = (b.x - a.x) * rx + (b.y - a.y) * ry;
            const F64 kept = std::clamp(side, -MAX_SHIFT_M, MAX_SHIFT_M);
            b.x += (kept - side) * rx;
            b.y += (kept - side) * ry;
        }
        // past the chain's end, its last direction carried on
        while (!out.empty() && (out.size() < 3 || (out.size() - 1) * PATH_STEP < length))
        {
            LanePoint q = out.back();
            q.x += q.ax * PATH_STEP;
            q.y += q.ay * PATH_STEP;
            out.push_back(q);
        }
        return out;
    }

    // The road read again: the lane line from the box walk (or a test course's), carried on along the road's
    // surface, smoothed, and weighed against the line kept. True where the path was laid (beforeAgentUpdate's
    // path frame moves on).
    bool readRoad(const LLVector3d& pos, F64 hx, F64 hy, F64 speed, F64 keep, F64 now, const std::string& course)
    {
        bool laid = false;
        sChainRoots.clear();
        sView.other_surface = 0;
        sView.chain_line.clear();
        sView.junctions.clear();
        sView.surface_changes.clear();
        sView.width_fallbacks = 0;
        sView.lane_kinks = 0;
        sView.edge_hooks = 0;
        sView.walk_end.clear();
        sView.route.clear();
        sView.road_end = false;
        sView.jump = false;
        sView.sign_speed_limit = 0.f;
        sView.sign_warning = 0;
        sPathFromStore = false;
        const auto walk_started = std::chrono::steady_clock::now();
        std::vector<LanePoint> p = std::string(course).empty()
                                       ? boxWalk(pos.mdV[VX], pos.mdV[VY], pos.mdV[VZ], hx, hy, std::max(V_PLAN_MIN, speed), keep)
                                       : host::testCourse(course, pos.mdV[VX], pos.mdV[VY], pos.mdV[VZ], hx, hy, std::max(V_PLAN_MIN, speed));
        sView.walk_ms = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - walk_started).count();
        sView.source = std::string(course).empty() ? "boxes" : "course " + std::string(course);
        const bool course_empty = std::string(course).empty();
        // the stored road's points are on the surface and smoothed already (StoredRoad): only what is added past
        // them here is
        size_t laid_n = course_empty ? p.size() : 0;
        if (p.empty())
        {
            sChainRoots.clear();
            p = joinPath(pos.mdV[VX], pos.mdV[VY], pos.mdV[VZ], hx, hy, std::max(V_PLAN_MIN, speed), keep);
            sView.source = "joins";
            laid_n = 0;
        }
        const bool from_store = laid_n > 0;
        if (!from_store)
        {
            sRoadPath.clear();
        }
        sView.chained = !p.empty();
        // every point on the road's surface beneath it, whichever way it was laid (a slab's height is its
        // faces' mean, kerbs and all; a walk past an open area carries its last height on)
        F64 zref = surfaceAt(pos.mdV[VX], pos.mdV[VY], pos.mdV[VZ] - 0.5);
        for (size_t i = 0; i < p.size(); ++i)
        {
            if (i < laid_n)
            {
                zref = p[i].z;
                continue;
            }
            p[i].z = zref = surfaceAt(p[i].x, p[i].y, zref);
        }
        // a walk that stopped short (one of its rules said no where the road plainly goes on): carried on along the
        // road's surface, a cross-section every PATH_STEP from its last point along its last way, each recentred
        // on the road there and turned no more than SURFACE_TURN_DEG nor moved across more than SURFACE_SHIFT_M a
        // step, until there is no road there or the line is long enough
        if (course_empty && p.size() >= 2 && !sView.road_end)
        {
            F64 have = 0;
            for (size_t i = 0; i + 1 < p.size(); ++i)
            {
                have += std::hypot(p[i + 1].x - p[i].x, p[i + 1].y - p[i].y);
            }
            const F64 want_m = lookAhead(std::max(V_PLAN_MIN, speed));
            F64 ax = p.back().ax, ay = p.back().ay;
            S32 added = 0;
            while (have < want_m && added < 80)
            {
                const LanePoint& last = p.back();
                LanePoint nx;
                if (!laneAt(last.x + ax * PATH_STEP, last.y + ay * PATH_STEP, last.z, ax, ay, keep, nx))
                {
                    break;
                }
                // its way within SURFACE_TURN_DEG of the line's, and no more than SURFACE_SHIFT_M across from straight on
                F64 nax = nx.ax, nay = nx.ay;
                const F64 cosd = nax * ax + nay * ay;
                if (cosd < std::cos(SURFACE_TURN_DEG * PI / 180))
                {
                    const F64 sgn = (ax * nay - ay * nax) >= 0 ? 1.0 : -1.0, t = SURFACE_TURN_DEG * PI / 180 * sgn;
                    nax = ax * std::cos(t) - ay * std::sin(t);
                    nay = ax * std::sin(t) + ay * std::cos(t);
                }
                const F64 sx = last.x + ax * PATH_STEP, sy = last.y + ay * PATH_STEP;
                const F64 across = (nx.x - sx) * ay - (nx.y - sy) * ax;       // + right
                const F64 move = std::clamp(across, -SURFACE_SHIFT_M, SURFACE_SHIFT_M);
                nx.x = sx + ay * move;
                nx.y = sy - ax * move;
                nx.ax = nax;
                nx.ay = nay;
                have += std::hypot(nx.x - last.x, nx.y - last.y);
                ax = nax;
                ay = nay;
                p.push_back(nx);
                ++added;
            }
            if (added)
            {
                sView.source += " + surface";
            }
        }
        // smoothed along its length, its first point where it is: laid a piece at a time, the line stepped
        // across the lane at the seams where pieces' centre lines differed (a staircase), which a [1 2 1] average,
        // twice, rounds into a gentle shift and leaves a bend all but untouched; a fresh line only (smoothing the
        // kept one every frame would drift it)
        if (from_store && p.size() > laid_n && p.size() >= 4 && laid_n > 0)
        {
            // the stored road's points are smoothed already: the points carried on past it, and the two stored ones
            // before the joint, as the whole line would be (from the stored points as laid)
            const size_t f = sStoreOutFirst;
            const size_t j0 = laid_n > 2 ? laid_n - 2 : 0, w0 = j0 > 2 ? j0 - 2 : 0;
            std::vector<LanePoint> work;
            for (size_t i = w0; i < p.size(); ++i)
            {
                work.push_back(i < laid_n && f + i < sStored.dense_raw.size() ? sStored.dense_raw[f + i] : p[i]);
            }
            smoothLine(work, 1);
            for (size_t i = j0; i < p.size(); ++i)
            {
                const LanePoint& q = work[i - w0];
                p[i].x = q.x;
                p[i].y = q.y;
                p[i].left_x = q.left_x;
                p[i].left_y = q.left_y;
                p[i].right_x = q.right_x;
                p[i].right_y = q.right_y;
            }
        }
        else if (course_empty && p.size() >= 4 && !from_store)
        {
            // the line and the road's edges alike (the edge brake reads the edges)
            smoothLine(p, 1);
        }
        if (!sChainRoots.empty())
        {
            // the route: kept, and grown while the chains agree with it (what it had ahead stays until passed)
            if (now - sRouteTime > ROUTE_KEEP_S)
            {
                sRoute.clear();
            }
            sRoute.insert(sChainRoots.begin(), sChainRoots.end());
            sRouteTime = now;
        }
        // the last path, what of it is still ahead of the car
        std::vector<LanePoint> kept;
        if (!sPath.empty() && now - sPathTime < PATH_KEEP_S)
        {
            size_t first = 0;
            while (first + 1 < sPath.size()
                   && (sPath[first + 1].x - pos.mdV[VX]) * sPath[first + 1].ax + (sPath[first + 1].y - pos.mdV[VY]) * sPath[first + 1].ay < 0)
            {
                ++first;
            }
            kept.assign(sPath.begin() + first, sPath.end());
        }
        // the road's end: nothing of the line lies past it, the kept line's no more than the fresh one's (the road
        // once found stays, but beyond its end there is none)
        if (sView.road_end)
        {
            auto past = [](const LanePoint& lp)
            { return (lp.x - sView.road_end_x) * sView.road_end_dx + (lp.y - sView.road_end_y) * sView.road_end_dy > 0.01; };
            while (!kept.empty() && past(kept.back()))
            {
                kept.pop_back();
            }
            while (p.size() > 1 && past(p.back()))
            {
                p.pop_back();
            }
        }
        // A new line that puts the car more than LANE_JUMP_M further to one side than the line it has (the walk
        // laid it in the other lane) is held off while the one it has still runs on ahead, unless the jump
        // stands for LANE_JUMP_S (a junction, a lane change, the road truly there): on a two-lane road the line
        // flipped 7-8 m between the lanes every second or so, the steering chased each flip with a hard hold,
        // and the swings carried the car over the edge of a cliff.
        bool jumped = false;
        if (!p.empty() && kept.size() >= 3 && course_empty)
        {
            const F64 off_new = sideOf(p, pos), off_kept = sideOf(kept, pos);
            if (std::fabs(off_new - off_kept) > LANE_JUMP_M)
            {
                if (sLaneJumpSince < 0)
                {
                    sLaneJumpSince = now;
                }
                jumped = now - sLaneJumpSince < LANE_JUMP_S;
            }
            else
            {
                sLaneJumpSince = -1.0;
            }
        }
        // a line whose way at the car points back more than LINE_BACK_DEG from the car's heading is a U-turn, never
        // the way on: refused (a line found again after a stretch with none ran the other way, and the car turned
        // round onto it)
        if (!p.empty() && course_empty && lineHeadingOff(p, pos, hx, hy) > LINE_BACK_DEG)
        {
            p.clear();
            sView.source = "refused (the line pointed back)";
        }
        // the kept line, unless the car has left it
        if (kept.size() >= 2 && std::fabs(sideOf(kept, pos)) > KEPT_OFF_M)
        {
            kept.clear();
        }
        if (!jumped && !p.empty() && kept.size() >= 3 && p.size() < kept.size() && course_empty)
        {
            // a fresh walk shorter than the line kept: it as far as it reaches, then the kept line on from its end
            // where they agree there; where they do not, the kept line stands
            const LanePoint& end = p.back();
            size_t join = kept.size();
            F64 best = KEPT_AGREE_M * KEPT_AGREE_M;
            for (size_t i = 0; i < kept.size(); ++i)
            {
                const F64 d = (kept[i].x - end.x) * (kept[i].x - end.x) + (kept[i].y - end.y) * (kept[i].y - end.y);
                if (d < best)
                {
                    best = d;
                    join = i;
                }
            }
            if (join < kept.size())
            {
                p.insert(p.end(), kept.begin() + join + 1, kept.end());
                sView.source = "boxes + kept";
            }
            else
            {
                p.clear();      // disagrees: the kept line stands (below)
            }
        }
        if (jumped)
        {
            sView.source = "kept (the new line jumped sideways)";
            sPath = std::move(kept);
            laid = true;
            sLastSeenRoad = now;
        }
        else if (!p.empty() && p.size() * 2 >= kept.size())
        {
            sLaneJumpSince = -1.0;
            sPath = std::move(p);
            laid = true;
            sPathTime = now;
            sLastSeenRoad = now;
            sPathFromStore = from_store;
        }
        else if (!kept.empty() && kept.size() >= 2)
        {
            // no fresh line, or one that disagrees: the road found stands, what is left of it ahead
            sView.source = "kept";
            sPath = std::move(kept);
            laid = true;
            sLastSeenRoad = now;
        }
        else if (!p.empty())
        {
            sPath = std::move(p);
            laid = true;
            sPathTime = now;
            sLastSeenRoad = now;
        }
        else
        {
            sPath.clear();
        }
        for (LanePoint& q : sPath)
        {
            applyKit(q, keep);
        }
        if (sPathFromStore)
        {
            for (LanePoint& q : sRoadPath)
            {
                applyKit(q, keep);
            }
        }
        else
        {
            sRoadPath.clear();
        }
        tagAccess();
        return laid;
    }

    // The good line's way at the car, for a square part's walk: noted once the path is laid, every frame.
    void noteLineWay(F64 now)
    {
        sNow = now;
        if (lineSeen() >= SIGHT_MIN_M)
        {
            // the good line's way at the car, for a square part's walk
            size_t k = std::min<size_t>(sPath.size() - 1, 4);
            const F64 lx = sPath[k].x - sPath[0].x, ly = sPath[k].y - sPath[0].y, L = std::hypot(lx, ly);
            if (L > 1.0)
            {
                sLineDirX = lx / L;
                sLineDirY = ly / L;
                sLineDirAt = now;
            }
        }
    }
}
