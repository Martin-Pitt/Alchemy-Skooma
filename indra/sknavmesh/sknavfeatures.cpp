/**
 * @file sknavfeatures.cpp
 * @brief Doors, ladders, elevators, vehicles and seats, recognised from census parts and what the viewer knows of them.
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

#include "sknavfeatures.h"

#include "llstring.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <unordered_map>

namespace
{
    using namespace sknav;

    struct Groups
    {
        std::vector<S32> mParent;

        explicit Groups(size_t n) : mParent(n)
        {
            std::iota(mParent.begin(), mParent.end(), 0);
        }

        S32 find(S32 i)
        {
            while (mParent[i] != i)
            {
                mParent[i] = mParent[mParent[i]];
                i = mParent[i];
            }
            return i;
        }

        bool unite(S32 a, S32 b)
        {
            a = find(a);
            b = find(b);
            if (a == b)
            {
                return false;
            }
            mParent[std::max(a, b)] = std::min(a, b);
            return true;
        }
    };

    void grow(LLVector3d& mn, LLVector3d& mx, const LLVector3d& bmn, const LLVector3d& bmx, bool first)
    {
        for (S32 k = 0; k < 3; ++k)
        {
            mn.mdV[k] = first ? bmn.mdV[k] : std::min(mn.mdV[k], bmn.mdV[k]);
            mx.mdV[k] = first ? bmx.mdV[k] : std::max(mx.mdV[k], bmx.mdV[k]);
        }
    }

    F64 planArea(const LLVector3d& mn, const LLVector3d& mx)
    {
        return std::max(0.0, mx.mdV[VX] - mn.mdV[VX]) * std::max(0.0, mx.mdV[VY] - mn.mdV[VY]);
    }

    F64 planOverlap(const FeaturePart& a, const FeaturePart& b)
    {
        const F64 w = std::min(a.mMax.mdV[VX], b.mMax.mdV[VX]) - std::max(a.mMin.mdV[VX], b.mMin.mdV[VX]);
        const F64 d = std::min(a.mMax.mdV[VY], b.mMax.mdV[VY]) - std::max(a.mMin.mdV[VY], b.mMin.mdV[VY]);
        return (w > 0.0 && d > 0.0) ? w * d : 0.0;
    }

    bool overlapsPlan(const LLVector3d& amn, const LLVector3d& amx, const LLVector3d& bmn, const LLVector3d& bmx)
    {
        return amn.mdV[VX] <= bmx.mdV[VX] && bmn.mdV[VX] <= amx.mdV[VX] && amn.mdV[VY] <= bmx.mdV[VY] && bmn.mdV[VY] <= amx.mdV[VY];
    }

    // Two ladder pieces on one footprint, touching or overlapping in height:
    // most of each plan in common, so a landing deck the ladder's top meets,
    // which holds the ladder's whole plan but is far larger, is not one.
    bool stacked(const FeaturePart& a, const FeaturePart& b, const FeatureParams& params)
    {
        if (a.mMin.mdV[VZ] > b.mMax.mdV[VZ] + params.mStackTolerance || b.mMin.mdV[VZ] > a.mMax.mdV[VZ] + params.mStackTolerance)
        {
            return false;
        }
        const F64 common = planOverlap(a, b);
        return common > 0.0 && common >= params.mFootprintOverlap * planArea(a.mMin, a.mMax)
            && common >= params.mFootprintOverlap * planArea(b.mMin, b.mMax);
    }

    // A thin upright panel: one axis stands vertical and is at least the class
    // height, and of the other two the thinner is no more than a leaf's
    // thickness while the wider makes it a panel rather than a post.
    bool thinPanel(const FeaturePart& p, const FeatureParams& params, LLVector3& normal, F32& thickness)
    {
        S32 up = -1;
        for (S32 k = 0; k < 3; ++k)
        {
            if (std::fabs(p.mAxes[k].mV[VZ]) >= 0.95f)
            {
                up = k;
            }
        }
        if (up < 0 || 2.f * p.mHalf.mV[up] < params.mClassHeight)
        {
            return false;
        }
        const S32 a = (up + 1) % 3, b = (up + 2) % 3;
        const S32 thin = p.mHalf.mV[a] <= p.mHalf.mV[b] ? a : b;
        const S32 wide = thin == a ? b : a;
        thickness = 2.f * p.mHalf.mV[thin];
        if (thickness > params.mThinMax || p.mHalf.mV[wide] < 2.f * p.mHalf.mV[thin])
        {
            return false;
        }
        normal = p.mAxes[thin];
        normal.mV[VZ] = 0.f;
        normal.normVec();
        return true;
    }
}

namespace sknav
{
    bool containsNoCase(const std::string& haystack, const char* needle)
    {
        const size_t n = std::strlen(needle);
        if (n == 0 || haystack.size() < n)
        {
            return n == 0;
        }
        for (size_t i = 0; i + n <= haystack.size(); ++i)
        {
            size_t k = 0;
            while (k < n && std::tolower(static_cast<unsigned char>(haystack[i + k])) == std::tolower(static_cast<unsigned char>(needle[k])))
            {
                ++k;
            }
            if (k == n)
            {
                return true;
            }
        }
        return false;
    }

    bool hoverShowsHealth(const std::string& hover)
    {
        for (size_t slash = hover.find('/'); slash != std::string::npos; slash = hover.find('/', slash + 1))
        {
            size_t l = slash;
            while (l > 0 && hover[l - 1] == ' ')
            {
                --l;
            }
            size_t r = slash + 1;
            while (r < hover.size() && hover[r] == ' ')
            {
                ++r;
            }
            if (l > 0 && std::isdigit(static_cast<unsigned char>(hover[l - 1])) && r < hover.size()
                && std::isdigit(static_cast<unsigned char>(hover[r])))
            {
                return true;
            }
        }
        return false;
    }

    U8 excludeFor(FeatureKind kind)
    {
        switch (kind)
        {
            case FeatureKind::DOOR: return EXCLUDE_AVATAR;
            case FeatureKind::ELEVATOR: return EXCLUDE_AVATAR;
            case FeatureKind::VEHICLE: return EXCLUDE_AVATAR | EXCLUDE_WEATHER;
            default: return 0;
        }
    }

    const char* featureName(FeatureKind kind)
    {
        switch (kind)
        {
            case FeatureKind::DOOR: return "door";
            case FeatureKind::LADDER: return "ladder";
            case FeatureKind::ELEVATOR: return "elevator";
            case FeatureKind::VEHICLE: return "vehicle";
            case FeatureKind::SEAT: return "seat";
            default: return "none";
        }
    }

    std::vector<Feature> classifyFeatures(const std::vector<FeaturePart>& parts, const std::vector<FeatureRoot>& roots,
                                          const FeatureParams& params)
    {
        std::vector<Feature> out;
        std::unordered_map<LLUUID, const FeatureRoot*> root_of;
        for (const FeatureRoot& r : roots)
        {
            root_of[r.mRoot] = &r;
        }
        auto rootFacts = [&](const LLUUID& id) -> const FeatureRoot* {
            const auto it = root_of.find(id);
            return it == root_of.end() ? nullptr : it->second;
        };
        std::vector<bool> claimed(parts.size(), false);

        // Vehicles and seats: a linkset someone has sat on this session. Whether
        // it has moved, or is physical, is what separates the two.
        for (const FeatureRoot& r : roots)
        {
            if (!r.mEverCrewed)
            {
                continue;
            }
            Feature f;
            f.mKind = (r.mEverMoved || r.mPhysical) ? FeatureKind::VEHICLE : FeatureKind::SEAT;
            f.mConfidence = 1.f;
            f.mRoots.push_back(r.mRoot);
            bool first = true;
            if (r.mHasBounds)
            {
                grow(f.mMin, f.mMax, r.mMin, r.mMax, true);
                first = false;
            }
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (parts[i].mRoot == r.mRoot)
                {
                    claimed[i] = true;
                    f.mParts.push_back(parts[i].mPart);
                    grow(f.mMin, f.mMax, parts[i].mMin, parts[i].mMax, first);
                    first = false;
                }
            }
            f.mEvidence = r.mEverMoved ? "crewed, moved" : (r.mPhysical ? "crewed, physical" : "crewed, never moved");
            if (f.mKind == FeatureKind::VEHICLE)
            {
                // LBA marks damageable walls as well as vehicles: it is a
                // property of a vehicle here, never the reason for one.
                f.mCombat = r.mDescription.compare(0, 6, "LBA.v.") == 0 || hoverShowsHealth(r.mHover);
                f.mAir = r.mClearance >= params.mAirClearance;
                if (f.mCombat)
                {
                    f.mEvidence += ", damageable";
                }
                if (f.mAir)
                {
                    f.mEvidence += llformat(", %.0f m up", r.mClearance);
                }
            }
            out.push_back(std::move(f));
        }

        // Ladders: the linkset name is the detector, and geometry only finds
        // the extent - the named pieces plus whatever sits stacked on their
        // footprint, across linksets, since a ladder is often built of several.
        {
            std::vector<S32> named;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                const FeatureRoot* r = rootFacts(parts[i].mRoot);
                if (!claimed[i] && r && containsNoCase(r->mName, "ladder"))
                {
                    named.push_back(static_cast<S32>(i));
                }
            }
            if (!named.empty())
            {
                Groups groups(parts.size());
                std::vector<bool> member(parts.size(), false);
                for (S32 i : named)
                {
                    member[i] = true;
                }
                for (size_t a = 1; a < named.size(); ++a)
                {
                    for (size_t b = 0; b < a; ++b)
                    {
                        if (parts[named[a]].mRoot == parts[named[b]].mRoot)
                        {
                            groups.unite(named[a], named[b]);
                        }
                    }
                }
                // Grow until nothing more joins: a piece stacked on a piece
                // stacked on the named one is still the same ladder.
                std::vector<S32> members = named;
                for (bool changed = true; changed;)
                {
                    changed = false;
                    const size_t count = members.size();
                    for (size_t m = 0; m < count; ++m)
                    {
                        for (size_t j = 0; j < parts.size(); ++j)
                        {
                            if (claimed[j] || static_cast<S32>(j) == members[m] || !stacked(parts[members[m]], parts[j], params))
                            {
                                continue;
                            }
                            if (groups.unite(members[m], static_cast<S32>(j)))
                            {
                                changed = true;
                            }
                            if (!member[j])
                            {
                                member[j] = true;
                                members.push_back(static_cast<S32>(j));
                            }
                        }
                    }
                }
                std::map<S32, Feature> ladders;
                for (S32 i : members)
                {
                    Feature& f = ladders[groups.find(i)];
                    const bool first = f.mParts.empty();
                    f.mKind = FeatureKind::LADDER;
                    f.mConfidence = 1.f;
                    f.mParts.push_back(parts[i].mPart);
                    if (std::find(f.mRoots.begin(), f.mRoots.end(), parts[i].mRoot) == f.mRoots.end())
                    {
                        f.mRoots.push_back(parts[i].mRoot);
                    }
                    grow(f.mMin, f.mMax, parts[i].mMin, parts[i].mMax, first);
                    claimed[i] = true;
                }
                for (auto& entry : ladders)
                {
                    Feature& f = entry.second;
                    f.mFoot = f.mMin.mdV[VZ];
                    f.mTop = f.mMax.mdV[VZ];
                    f.mEvidence = llformat("named, %zu pieces in %zu linksets", f.mParts.size(), f.mRoots.size());
                    out.push_back(std::move(f));
                }
            }
        }

        // A root named as an elevator's car is never a door, whatever its cab
        // is built of; whether it is a car is settled by the shaft below.
        auto carName = [](const FeatureRoot* r) {
            return r && (containsNoCase(r->mName, "elevator") || containsNoCase(r->mName, "lift"));
        };

        // Doors: a thin upright panel with some evidence it opens. A thin
        // panel with nothing to say for it is a wall, and one named as a
        // window is a pane, however door-shaped.
        struct Leaf
        {
            S32 mPart;
            LLVector3 mNormal;
            F32 mThickness;
            F32 mConfidence;
            std::string mEvidence;
        };
        std::vector<Leaf> leaves;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            const FeaturePart& p = parts[i];
            const FeatureRoot* r = rootFacts(p.mRoot);
            LLVector3 normal;
            F32 thickness = 0.f;
            if (claimed[i] || carName(r) || !thinPanel(p, params, normal, thickness))
            {
                continue;
            }
            const std::string root_name = r ? r->mName : std::string();
            if (containsNoCase(p.mName, "window") || containsNoCase(p.mName, "glass") || containsNoCase(root_name, "window")
                || containsNoCase(root_name, "glass"))
            {
                continue;
            }
            F32 confidence = 0.f;
            std::string evidence;
            auto add = [&](F32 weight, const char* why) {
                confidence += weight;
                evidence += evidence.empty() ? why : std::string(", ") + why;
            };
            if (containsNoCase(p.mName, "door") || containsNoCase(root_name, "door"))
            {
                add(0.5f, "named");
            }
            if (p.mScripted || (r && r->mScripted == TriState::YES))
            {
                add(0.25f, "scripted");
            }
            if (p.mTouch)
            {
                add(0.15f, "touch");
            }
            if (r && r->mEverMoved)
            {
                add(0.3f, "moved");
            }
            if (evidence.empty())
            {
                continue;
            }
            leaves.push_back({ static_cast<S32>(i), normal, thickness, std::min(confidence, 1.f), evidence });
        }

        // Two leaves side by side in one plane, touching, are one double door.
        Groups pairs(leaves.size());
        for (size_t a = 0; a < leaves.size(); ++a)
        {
            for (size_t b = a + 1; b < leaves.size(); ++b)
            {
                const FeaturePart& pa = parts[leaves[a].mPart];
                const FeaturePart& pb = parts[leaves[b].mPart];
                if (std::fabs(leaves[a].mNormal * leaves[b].mNormal) < 0.98f)
                {
                    continue;
                }
                const LLVector3 d(pb.mCenter - pa.mCenter);
                const F32 off_plane = std::fabs(d * leaves[a].mNormal);
                const LLVector3d slack(0.05, 0.05, 0.05);
                const F64 shared_z = std::min(pa.mMax.mdV[VZ], pb.mMax.mdV[VZ]) - std::max(pa.mMin.mdV[VZ], pb.mMin.mdV[VZ]);
                const F64 shorter = std::min(pa.mMax.mdV[VZ] - pa.mMin.mdV[VZ], pb.mMax.mdV[VZ] - pb.mMin.mdV[VZ]);
                if (off_plane <= 0.1f + std::max(leaves[a].mThickness, leaves[b].mThickness)
                    && overlapsPlan(pa.mMin - slack, pa.mMax + slack, pb.mMin, pb.mMax) && shared_z >= 0.5 * shorter)
                {
                    pairs.unite(static_cast<S32>(a), static_cast<S32>(b));
                }
            }
        }
        std::map<S32, S32> door_of;     // leaf group -> index into out
        const size_t first_door = out.size();
        for (size_t a = 0; a < leaves.size(); ++a)
        {
            const S32 g = pairs.find(static_cast<S32>(a));
            auto it = door_of.find(g);
            if (it == door_of.end())
            {
                it = door_of.emplace(g, static_cast<S32>(out.size())).first;
                Feature f;
                f.mKind = FeatureKind::DOOR;
                f.mNormal = leaves[a].mNormal;
                f.mEvidence = leaves[a].mEvidence;
                out.push_back(std::move(f));
            }
            Feature& f = out[it->second];
            const FeaturePart& p = parts[leaves[a].mPart];
            grow(f.mMin, f.mMax, p.mMin, p.mMax, f.mParts.empty());
            f.mParts.push_back(p.mPart);
            if (std::find(f.mRoots.begin(), f.mRoots.end(), p.mRoot) == f.mRoots.end())
            {
                f.mRoots.push_back(p.mRoot);
            }
            f.mConfidence = std::max(f.mConfidence, leaves[a].mConfidence);
            claimed[leaves[a].mPart] = true;
        }
        const size_t door_count = out.size() - first_door;

        // Shafts: doors stacked on one vertical line, a storey or more apart.
        Groups stacks(door_count);
        for (size_t a = 0; a < door_count; ++a)
        {
            for (size_t b = a + 1; b < door_count; ++b)
            {
                const Feature& da = out[first_door + a];
                const Feature& db = out[first_door + b];
                if (std::fabs(da.mNormal * db.mNormal) < 0.98f)
                {
                    continue;
                }
                const LLVector3d ca = (da.mMin + da.mMax) * 0.5, cb = (db.mMin + db.mMax) * 0.5;
                const F64 plan = std::hypot(ca.mdV[VX] - cb.mdV[VX], ca.mdV[VY] - cb.mdV[VY]);
                if (plan <= params.mDoorLine && std::fabs(ca.mdV[VZ] - cb.mdV[VZ]) >= params.mClassHeight)
                {
                    stacks.unite(static_cast<S32>(a), static_cast<S32>(b));
                }
            }
        }
        std::map<S32, std::vector<S32>> shafts;
        for (size_t a = 0; a < door_count; ++a)
        {
            shafts[stacks.find(static_cast<S32>(a))].push_back(static_cast<S32>(first_door + a));
        }
        for (const auto& entry : shafts)
        {
            if (entry.second.size() < 2)
            {
                continue;
            }
            Feature shaft;
            shaft.mKind = FeatureKind::ELEVATOR;
            shaft.mDoors = entry.second;
            LLVector3d dmin, dmax;
            for (size_t k = 0; k < entry.second.size(); ++k)
            {
                const Feature& d = out[entry.second[k]];
                grow(dmin, dmax, d.mMin, d.mMax, k == 0);
            }
            shaft.mNormal = out[entry.second.front()].mNormal;
            // Where a car could be: the door line swept a few metres either
            // way along the doors' normal, since which side the column is on
            // is what the car says.
            const LLVector3d reach(std::fabs(shaft.mNormal.mV[VX]) * params.mCarReach, std::fabs(shaft.mNormal.mV[VY]) * params.mCarReach, 0.0);
            const LLVector3d smin = dmin - reach, smax = dmax + reach;

            // A working elevator needs a scripted car named as one in the column.
            const FeatureRoot* car = nullptr;
            LLVector3d cmin, cmax;
            std::vector<S32> car_parts;
            for (const FeatureRoot& r : roots)
            {
                if (!carName(&r))
                {
                    continue;
                }
                std::vector<S32> mine;
                bool scripted = r.mScripted == TriState::YES || r.mWorkingElevator;
                LLVector3d bmin = r.mMin, bmax = r.mMax;
                bool have = r.mHasBounds;
                for (size_t i = 0; i < parts.size(); ++i)
                {
                    if (parts[i].mRoot == r.mRoot && !claimed[i])
                    {
                        mine.push_back(static_cast<S32>(i));
                        scripted = scripted || parts[i].mScripted;
                        grow(bmin, bmax, parts[i].mMin, parts[i].mMax, !have);
                        have = true;
                    }
                }
                if (scripted && have && overlapsPlan(bmin, bmax, smin, smax))
                {
                    car = &r;
                    cmin = bmin;
                    cmax = bmax;
                    car_parts = mine;
                    break;
                }
            }
            if (car)
            {
                shaft.mWorking = true;
                shaft.mConfidence = 1.f;
                shaft.mRoots.push_back(car->mRoot);
                for (S32 i : car_parts)
                {
                    shaft.mParts.push_back(parts[i].mPart);
                    claimed[i] = true;
                }
                // The column is the car's own footprint, not the void walls
                // bound: two shafts side by side can share one void up top.
                shaft.mMin.set(cmin.mdV[VX], cmin.mdV[VY], std::min(dmin.mdV[VZ], cmin.mdV[VZ]));
                shaft.mMax.set(cmax.mdV[VX], cmax.mdV[VY], std::max(dmax.mdV[VZ], cmax.mdV[VZ]));
                shaft.mEvidence = llformat("%zu stacked doors, car", entry.second.size());
            }
            else
            {
                shaft.mConfidence = 0.5f;
                shaft.mMin = dmin;
                shaft.mMax = dmax;
                shaft.mEvidence = llformat("%zu stacked doors, no car", entry.second.size());
            }
            shaft.mFoot = shaft.mMin.mdV[VZ];
            shaft.mTop = shaft.mMax.mdV[VZ];
            out.push_back(std::move(shaft));
        }
        return out;
    }
}
