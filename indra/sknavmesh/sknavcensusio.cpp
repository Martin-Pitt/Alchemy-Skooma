/**
 * @file sknavcensusio.cpp
 * @brief A census export: everything the navmesh builds from, to a file and back, so a place replays offline.
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

#include "sknavcensusio.h"

#include "sknavannotate.h"

#include "llbase64.h"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <ostream>
#include <sstream>

namespace sknav
{
    // Whoever adds a field to Record packs and unpacks it below, then updates this size.
    static_assert(sizeof(void*) != 8 || sizeof(Record) == 224, "a new Record field must go into the census export");
    // And a TileBuildInput field goes into the params, unless a tile sets it.
    static_assert(sizeof(void*) != 8 || sizeof(TileBuildInput) == 232, "a new TileBuildInput field must go into the census export");
    // And a CensusRoot field goes into writeRootTable and the reader.
    static_assert(sizeof(void*) != 8 || sizeof(CensusRoot) == sizeof(LLUUID) + 2 * sizeof(std::string) + 8, "a new CensusRoot field must go into the root table");

    namespace
    {
        const char* const FORMAT = "sknav-census";
        constexpr U32 NO_TRIANGLES = 0xffffffffu;

        /// The packed record, in the order written; the file names it the same way.
        const char* const RECORD_LAYOUT =
            "[[\"root\",\"16s\"],[\"part\",\"16s\"],[\"role\",\"B\"],[\"provenance\",\"B\"],[\"nav_role\",\"B\"],[\"shape\",\"B\"],"
            "[\"dynamic\",\"B\"],[\"solid\",\"B\"],[\"feature\",\"B\"],[\"exclude\",\"B\"],[\"origin\",\"3d\"],[\"center\",\"3f\"],"
            "[\"axes\",\"9f\"],[\"half\",\"3f\"],[\"min\",\"3d\"],[\"max\",\"3d\"],[\"signature\",\"Q\"],[\"tri_first\",\"I\"],"
            "[\"tri_count\",\"I\"],[\"convex_first\",\"I\"],[\"convex_count\",\"I\"]]";
        constexpr size_t RECORD_BYTES = 196;

        template<typename T>
        void put(std::vector<U8>& out, const T& v)
        {
            const U8* p = reinterpret_cast<const U8*>(&v);
            out.insert(out.end(), p, p + sizeof(T));
        }

        struct Cursor
        {
            const U8* mData;
            size_t mSize;
            size_t mPos = 0;

            template<typename T>
            T get()
            {
                T v{};
                if (mPos + sizeof(T) <= mSize)
                {
                    std::memcpy(&v, mData + mPos, sizeof(T));
                }
                mPos += sizeof(T);
                return v;
            }
            bool ok() const { return mPos <= mSize; }
        };

        std::string b64(const std::vector<U8>& bytes)
        {
            return LLBase64::encode(bytes.data(), bytes.size());
        }

        template<typename T>
        std::string b64(const std::vector<T>& v)
        {
            return LLBase64::encode(reinterpret_cast<const U8*>(v.data()), v.size() * sizeof(T));
        }

        void vec(std::ostream& out, const LLVector3d& v)
        {
            out << '[' << v.mdV[VX] << ',' << v.mdV[VY] << ',' << v.mdV[VZ] << ']';
        }

        void vec(std::ostream& out, const LLVector3& v)
        {
            out << '[' << v.mV[VX] << ',' << v.mV[VY] << ',' << v.mV[VZ] << ']';
        }

        void ids(std::ostream& out, const std::vector<LLUUID>& v)
        {
            out << '[';
            for (size_t i = 0; i < v.size(); ++i)
            {
                out << (i ? ",\"" : "\"") << v[i] << '"';
            }
            out << ']';
        }

        const char* boolText(bool b)
        {
            return b ? "true" : "false";
        }

        // ------------------------------------------------------------ reading

        typedef simdjson::dom::element Elem;

        /// Lookups that remember the first thing missing, so one check at the end says it.
        struct Reader
        {
            std::string mError;

            bool fail(const std::string& what)
            {
                if (mError.empty())
                {
                    mError = what;
                }
                return false;
            }

            bool member(const Elem& obj, const char* key, Elem& out)
            {
                if (obj[key].get(out) != simdjson::SUCCESS)
                {
                    return fail(std::string("missing ") + key);
                }
                return true;
            }

            F64 num(const Elem& obj, const char* key)
            {
                Elem e;
                F64 v = 0.0;
                if (member(obj, key, e) && e.get_double().get(v) != simdjson::SUCCESS)
                {
                    fail(std::string("not a number: ") + key);
                }
                return v;
            }

            F32 f32(const Elem& obj, const char* key) { return static_cast<F32>(num(obj, key)); }

            bool flag(const Elem& obj, const char* key)
            {
                Elem e;
                bool v = false;
                if (member(obj, key, e) && e.get_bool().get(v) != simdjson::SUCCESS)
                {
                    fail(std::string("not a bool: ") + key);
                }
                return v;
            }

            std::string text(const Elem& obj, const char* key)
            {
                Elem e;
                std::string_view v;
                if (member(obj, key, e) && e.get_string().get(v) != simdjson::SUCCESS)
                {
                    fail(std::string("not a string: ") + key);
                }
                return std::string(v);
            }

            LLUUID id(const Elem& obj, const char* key) { return LLUUID(text(obj, key)); }

            simdjson::dom::array array(const Elem& obj, const char* key)
            {
                Elem e;
                simdjson::dom::array a;
                if (member(obj, key, e) && e.get_array().get(a) != simdjson::SUCCESS)
                {
                    fail(std::string("not an array: ") + key);
                }
                return a;
            }

            F64 at(const simdjson::dom::array& a, size_t i)
            {
                F64 v = 0.0;
                if (a.at(i).get_double().get(v) != simdjson::SUCCESS)
                {
                    fail("short vector");
                }
                return v;
            }

            LLVector3d vec3d(const Elem& obj, const char* key)
            {
                const simdjson::dom::array a = array(obj, key);
                return mError.empty() ? LLVector3d(at(a, 0), at(a, 1), at(a, 2)) : LLVector3d();
            }

            LLVector3 vec3(const Elem& obj, const char* key)
            {
                const LLVector3d v = vec3d(obj, key);
                return LLVector3(static_cast<F32>(v.mdV[VX]), static_cast<F32>(v.mdV[VY]), static_cast<F32>(v.mdV[VZ]));
            }

            LLVector3d vec3d(const Elem& e)
            {
                simdjson::dom::array a;
                if (e.get_array().get(a) != simdjson::SUCCESS)
                {
                    fail("not a vector");
                    return LLVector3d();
                }
                return LLVector3d(at(a, 0), at(a, 1), at(a, 2));
            }

            std::vector<LLUUID> idList(const Elem& obj, const char* key)
            {
                std::vector<LLUUID> out;
                for (Elem e : array(obj, key))
                {
                    std::string_view v;
                    if (e.get_string().get(v) != simdjson::SUCCESS)
                    {
                        fail(std::string("not an id: ") + key);
                        break;
                    }
                    out.emplace_back(std::string(v));
                }
                return out;
            }

            std::string blob(const Elem& obj, const char* key)
            {
                const std::string encoded = text(obj, key);
                if (encoded.empty())
                {
                    return {};
                }
                std::string bytes = LLBase64::decodeAsString(encoded);
                if (bytes.empty())
                {
                    fail(std::string("bad base64: ") + key);
                }
                return bytes;
            }

            template<typename T>
            std::vector<T> packed(const Elem& obj, const char* key)
            {
                const std::string bytes = blob(obj, key);
                std::vector<T> out;
                if (bytes.size() % sizeof(T))
                {
                    fail(std::string("ragged blob: ") + key);
                    return out;
                }
                out.resize(bytes.size() / sizeof(T));
                if (!out.empty())
                {
                    std::memcpy(out.data(), bytes.data(), bytes.size());
                }
                return out;
            }
        };
    }

    bool CensusRegion::contains(F64 x, F64 y) const
    {
        // As LLViewerRegion::pointInRegionGlobal: region-local F32, half open.
        const F32 lx = static_cast<F32>(x - mOrigin.mdV[VX]);
        const F32 ly = static_cast<F32>(y - mOrigin.mdV[VY]);
        return lx >= 0.f && lx < mWidth && ly >= 0.f && ly < mWidth;
    }

    bool CensusRegion::heightAt(F64 x, F64 y, F32& z) const
    {
        const F64 fx = x - mOrigin.mdV[VX] - mPhase;
        const F64 fy = y - mOrigin.mdV[VY] - mPhase;
        const F64 ix = std::floor(fx + 0.5), iy = std::floor(fy + 0.5);
        if (std::fabs(fx - ix) > 1e-4 || std::fabs(fy - iy) > 1e-4 || ix < 0 || iy < 0 || ix >= mSamples || iy >= mSamples)
        {
            return false;
        }
        z = mZ[static_cast<size_t>(iy) * mSamples + static_cast<size_t>(ix)];
        return true;
    }

    const CensusRegion* CensusFile::regionAt(F64 x, F64 y) const
    {
        for (const CensusRegion& r : mRegions)
        {
            if (r.contains(x, y))
            {
                return &r;
            }
        }
        return nullptr;
    }

    const CensusRoot* CensusFile::rootOf(const LLUUID& id) const
    {
        for (const CensusRoot& r : mRoots)
        {
            if (r.mId == id)
            {
                return &r;
            }
        }
        return nullptr;
    }

    const char* nameSourceName(NameSource source)
    {
        switch (source)
        {
            case NameSource::LINKSET: return "linkset";
            case NameSource::PROPERTIES: return "properties";
            default: return "none";
        }
    }

    std::string capDescription(const std::string& text)
    {
        if (text.size() <= ROOT_DESCRIPTION_MAX)
        {
            return text;
        }
        size_t end = ROOT_DESCRIPTION_MAX;
        // Back off continuation bytes so the cut lands on a character's first byte.
        while (end > 0 && (static_cast<U8>(text[end]) & 0xc0) == 0x80)
        {
            --end;
        }
        return text.substr(0, end);
    }

    std::vector<RootShare> dominantRoots(const CensusFile& census, const CensusCluster& cluster, size_t limit)
    {
        std::map<LLUUID, U32> counts;
        for (U32 i : cluster.mRecords)
        {
            if (i < census.mRecords.size())
            {
                ++counts[census.mRecords[i]->mRoot];
            }
        }
        std::vector<RootShare> out;
        out.reserve(counts.size());
        for (const auto& c : counts)
        {
            out.push_back({ c.first, c.second });
        }
        std::stable_sort(out.begin(), out.end(), [](const RootShare& a, const RootShare& b) { return a.mParts > b.mParts; });
        if (out.size() > limit)
        {
            out.resize(limit);
        }
        return out;
    }

    namespace
    {
        template<typename Lookup>
        std::string labelOf(const std::vector<RootShare>& all, Lookup root_of)
        {
            if (all.empty())
            {
                return {};
            }
            std::string name;
            for (const RootShare& s : all)
            {
                const CensusRoot* r = root_of(s.mId);
                if (r && !r->mName.empty())
                {
                    name = r->mName;
                    break;
                }
            }
            if (name.empty())
            {
                name = all.front().mId.asString();
            }
            return all.size() > 1 ? name + " (+" + std::to_string(all.size() - 1) + ")" : name;
        }
    }

    std::string clusterLabel(const CensusFile& census, const CensusCluster& cluster)
    {
        return labelOf(dominantRoots(census, cluster, std::numeric_limits<size_t>::max()),
                       [&census](const LLUUID& id) { return census.rootOf(id); });
    }

    void writeRootTable(std::ostream& out, const std::vector<CensusRoot>& roots)
    {
        out << '[';
        for (size_t i = 0; i < roots.size(); ++i)
        {
            const CensusRoot& r = roots[i];
            out << (i ? "," : "") << "{\"id\":\"" << r.mId << "\",\"name\":\"" << jsonEscape(r.mName) << "\",\"description\":\""
                << jsonEscape(capDescription(r.mDescription)) << "\",\"scripted\":" << (r.mScriptedKnown ? boolText(r.mScripted) : "null")
                << ",\"source\":\"" << nameSourceName(r.mSource) << "\"}";
        }
        out << ']';
    }

    void writeCensus(std::ostream& out, const CensusFile& c)
    {
        out.precision(std::numeric_limits<F64>::max_digits10);
        out << "{\"format\":\"" << FORMAT << "\",\"version\":" << CENSUS_FORMAT_VERSION << ",\"exported\":\"" << jsonEscape(c.mExported)
            << "\",\"stamp\":" << c.mStamp << ",\"agent\":";
        vec(out, c.mAgent);
        out << ",\"camera\":";
        vec(out, c.mCamera);
        out << ",\"range\":" << c.mRange;

        const TileBuildInput& p = c.mParams;
        out << ",\"params\":{\"tile_size\":" << p.mTileSize << ",\"cell\":" << p.mCell << ",\"partition\":" << static_cast<S32>(p.mPartition)
            << ",\"detail_sample_cells\":" << p.mDetailSampleCells << ",\"detail_max_error_cells\":" << p.mDetailMaxErrorCells
            << ",\"wall_min_height\":" << p.mWallMinHeight << ",\"wall_faces\":" << boolText(p.mWallFaces) << ",\"drop_spacing\":" << p.mDropSpacing
            << ",\"air\":" << boolText(p.mAir) << ",\"air_pad\":" << p.mAirPad << ",\"air_min_gap\":" << p.mAirMinGap << ",\"classes\":[";
        for (S32 k = 0; k < NUM_CLASSES; ++k)
        {
            const ClassParams& cp = p.mParams[k];
            out << (k ? "," : "") << "{\"name\":\"" << (k ? "weather" : "avatar") << "\",\"built\":" << boolText(p.mClasses[k])
                << ",\"height\":" << cp.mHeight << ",\"radius\":" << cp.mRadius << ",\"climb\":" << cp.mClimb << ",\"slope\":" << cp.mSlope
                << ",\"edges\":" << boolText(p.mEdges[k]) << ",\"drop_max\":" << p.mDropMax[k] << '}';
        }
        out << "]}";

        const EnclosureParams& e = c.mEnclosure;
        out << ",\"settings\":{\"air_radius\":" << c.mAirRadius << ",\"build_weather\":" << boolText(c.mBuildWeather) << ",\"cluster_gap\":" << c.mClusterGap << ",\"grounded_margin\":" << c.mGroundedMargin
            << ",\"void_margin\":" << c.mVoidMargin << "},\"enclosure\":{\"sky_decay\":" << e.mSkyDecayM << ",\"depth_full\":" << e.mDepthFullM
            << ",\"climb\":" << e.mClimbM << ",\"walk_head\":" << e.mWalkHeadM << ",\"walk_overlap\":" << e.mWalkOverlapM
            << ",\"carry_overlap\":" << e.mCarryOverlapM << ",\"carry_rise\":" << e.mCarryRiseM << ",\"sky_reach\":" << e.mSkyReachM
            << ",\"hysteresis\":" << e.mHysteresis << ",\"sealed_indoors\":" << boolText(e.mSealedIndoors) << '}';

        out << ",\"regions\":[";
        for (size_t i = 0; i < c.mRegions.size(); ++i)
        {
            const CensusRegion& r = c.mRegions[i];
            out << (i ? "," : "") << "{\"id\":\"" << r.mId << "\",\"handle\":\"" << r.mHandle << "\",\"name\":\"" << jsonEscape(r.mName) << "\",\"origin\":";
            vec(out, r.mOrigin);
            out << ",\"width\":" << r.mWidth << ",\"water\":" << r.mWater << ",\"land\":[" << r.mLandMin << ',' << r.mLandMax
                << "],\"phase\":" << r.mPhase << ",\"samples\":" << r.mSamples << ",\"z\":\"" << b64(r.mZ) << "\"}";
        }
        out << ']';

        // Records packed, triangles shared by pointer stay shared.
        std::vector<U8> records;
        records.reserve(c.mRecords.size() * RECORD_BYTES);
        std::vector<LLVector3> vertices;
        std::vector<S32> convex;
        std::map<const std::vector<LLVector3>*, U32> soups;
        std::map<const Record*, U32> index;
        for (const RecordPtr& rp : c.mRecords)
        {
            const Record& r = *rp;
            index[&r] = static_cast<U32>(index.size());
            records.insert(records.end(), r.mRoot.mData, r.mRoot.mData + UUID_BYTES);
            records.insert(records.end(), r.mPart.mData, r.mPart.mData + UUID_BYTES);
            put(records, static_cast<U8>(r.mRole));
            put(records, static_cast<U8>(r.mProvenance));
            put(records, static_cast<U8>(r.mNavRole));
            put(records, static_cast<U8>(r.mShape));
            put(records, static_cast<U8>(r.mDynamic));
            put(records, static_cast<U8>(r.mSolid));
            put(records, static_cast<U8>(r.mFeature));
            put(records, r.mExclude);
            put(records, r.mOrigin.mdV);
            put(records, r.mCenter.mV);
            for (const LLVector3& a : r.mAxes)
            {
                put(records, a.mV);
            }
            put(records, r.mHalf.mV);
            put(records, r.mMin.mdV);
            put(records, r.mMax.mdV);
            put(records, r.mSignature);
            U32 first = NO_TRIANGLES, count = 0;
            if (r.mTriangles)
            {
                const auto found = soups.find(r.mTriangles.get());
                if (found != soups.end())
                {
                    first = found->second;
                }
                else
                {
                    first = static_cast<U32>(vertices.size());
                    soups[r.mTriangles.get()] = first;
                    vertices.insert(vertices.end(), r.mTriangles->begin(), r.mTriangles->end());
                }
                count = static_cast<U32>(r.mTriangles->size());
            }
            put(records, first);
            put(records, count);
            put(records, static_cast<U32>(convex.size()));
            put(records, static_cast<U32>(r.mConvexTris.size()));
            convex.insert(convex.end(), r.mConvexTris.begin(), r.mConvexTris.end());
        }
        out << ",\"layout\":{\"record\":" << RECORD_LAYOUT << ",\"record_bytes\":" << RECORD_BYTES
            << ",\"vertex\":\"3f, region-local\",\"convex\":\"i, triangles per body\",\"cluster_records\":\"I, record index\","
               "\"terrain\":\"f, global z, row-major along y\",\"no_triangles\":"
            << NO_TRIANGLES << "},\"records\":{\"count\":" << c.mRecords.size() << ",\"blob\":\"" << b64(records) << "\",\"vertices\":\""
            << b64(vertices) << "\",\"convex\":\"" << b64(convex) << "\"}";

        std::map<LLUUID, const CensusRoot*> roots;
        for (const CensusRoot& r : c.mRoots)
        {
            roots.emplace(r.mId, &r);
        }
        const auto root_of = [&roots](const LLUUID& id) -> const CensusRoot* {
            const auto found = roots.find(id);
            return found == roots.end() ? nullptr : found->second;
        };
        out << ",\"clusters\":[";
        for (size_t i = 0; i < c.mClusters.size(); ++i)
        {
            const CensusCluster& cl = c.mClusters[i];
            const std::vector<RootShare> all = dominantRoots(c, cl, std::numeric_limits<size_t>::max());
            out << (i ? "," : "") << "{\"id\":\"" << cl.mId << "\",\"grounded\":" << boolText(cl.mGrounded) << ",\"records\":\"" << b64(cl.mRecords)
                << "\",\"label\":\"" << jsonEscape(labelOf(all, root_of)) << "\",\"top_roots\":[";
            // Derived from the records and the root table, so the reader skips it.
            for (size_t k = 0; k < all.size() && k < 3; ++k)
            {
                const RootShare& share = all[k];
                const CensusRoot* r = root_of(share.mId);
                out << (k ? "," : "") << "{\"id\":\"" << share.mId << "\",\"parts\":" << share.mParts << ",\"name\":\""
                    << jsonEscape(r ? r->mName : std::string()) << "\"}";
            }
            out << "]}";
        }
        out << "],\"roots\":";
        writeRootTable(out, c.mRoots);

        out << ",\"features\":[";
        for (size_t i = 0; i < c.mFeatures.size(); ++i)
        {
            const Feature& f = c.mFeatures[i];
            out << (i ? "," : "") << "{\"kind\":" << static_cast<S32>(f.mKind) << ",\"name\":\"" << featureName(f.mKind)
                << "\",\"confidence\":" << f.mConfidence << ",\"min\":";
            vec(out, f.mMin);
            out << ",\"max\":";
            vec(out, f.mMax);
            out << ",\"foot\":" << f.mFoot << ",\"top\":" << f.mTop << ",\"normal\":";
            vec(out, f.mNormal);
            out << ",\"working\":" << boolText(f.mWorking) << ",\"combat\":" << boolText(f.mCombat) << ",\"air\":" << boolText(f.mAir)
                << ",\"evidence\":\"" << jsonEscape(f.mEvidence) << "\",\"doors\":[";
            for (size_t k = 0; k < f.mDoors.size(); ++k)
            {
                out << (k ? "," : "") << f.mDoors[k];
            }
            out << "],\"roots\":";
            ids(out, f.mRoots);
            out << ",\"parts\":";
            ids(out, f.mParts);
            out << '}';
        }
        out << ']';

        out << ",\"doors\":[";
        for (size_t i = 0; i < c.mDoors.size(); ++i)
        {
            const DoorEntity& d = c.mDoors[i];
            out << (i ? "," : "") << "{\"id\":\"" << d.mId << "\",\"region\":\"" << d.mRegion << "\",\"min\":";
            vec(out, d.mMin);
            out << ",\"max\":";
            vec(out, d.mMax);
            out << ",\"plane\":";
            vec(out, d.mPlanePoint);
            out << ",\"planeKnown\":" << boolText(d.mPlaneKnown) << ",\"normal\":";
            vec(out, d.mNormal);
            out << ",\"width\":" << d.mWidth << ",\"height\":" << d.mHeight << ",\"sill\":" << d.mSill << ",\"double\":" << boolText(d.mDouble)
                << ",\"confidence\":" << d.mConfidence << ",\"damageable\":" << boolText(d.mDamageable) << ",\"name\":\"" << jsonEscape(d.mName)
                << "\",\"leaves\":";
            ids(out, d.mLeaves);
            out << '}';
        }
        out << ']';

        out << ",\"spans\":[";
        for (size_t i = 0; i < c.mSpans.size(); ++i)
        {
            const CensusSpan& s = c.mSpans[i];
            out << (i ? "," : "") << "{\"id\":\"" << s.mId << "\",\"kind\":" << static_cast<S32>(s.mKind) << ",\"min\":";
            vec(out, s.mMin);
            out << ",\"max\":";
            vec(out, s.mMax);
            out << ",\"classes\":[";
            for (S32 k = 0; k < NUM_CLASSES; ++k)
            {
                out << (k ? "," : "") << "{\"exits\":[";
                for (size_t x = 0; x < s.mExits[k].size(); ++x)
                {
                    out << (x ? "," : "");
                    vec(out, s.mExits[k][x]);
                }
                out << "],\"links\":[";
                for (size_t x = 0; x < s.mLinks[k].size(); ++x)
                {
                    out << (x ? "," : "") << '[';
                    vec(out, s.mLinks[k][x].first);
                    out << ',';
                    vec(out, s.mLinks[k][x].second);
                    out << ']';
                }
                out << "]}";
            }
            out << "]}";
        }
        out << "],";
        writeMarks(out, c.mMarks, [&c](const LLVector3d& v, MarkPlace& place) {
            const CensusRegion* r = c.regionAt(v.mdV[VX], v.mdV[VY]);
            if (!r)
            {
                return false;
            }
            place.mRegion = r->mName;
            place.mGridX = static_cast<U32>(r->mHandle >> 32) / 256;
            place.mGridY = static_cast<U32>(r->mHandle & 0xffffffff) / 256;
            place.mLocal.set(static_cast<F32>(v.mdV[VX] - r->mOrigin.mdV[VX]), static_cast<F32>(v.mdV[VY] - r->mOrigin.mdV[VY]),
                             static_cast<F32>(v.mdV[VZ] - r->mOrigin.mdV[VZ]));
            return true;
        });
        out << "}\n";
    }

    bool readCensus(const std::string& text, CensusFile& c, std::string* error)
    {
        simdjson::dom::parser parser;
        const simdjson::padded_string padded(text);
        Elem doc;
        if (parser.parse(padded).get(doc) != simdjson::SUCCESS)
        {
            if (error)
            {
                *error = "not JSON";
            }
            return false;
        }
        Reader rd;
        c = CensusFile();
        const U32 version = static_cast<U32>(rd.num(doc, "version"));
        if (rd.text(doc, "format") != FORMAT || version < 1 || version > CENSUS_FORMAT_VERSION)
        {
            if (error)
            {
                *error = "not a census of this version";
            }
            return false;
        }
        c.mExported = rd.text(doc, "exported");
        c.mStamp = static_cast<U64>(rd.num(doc, "stamp"));
        c.mAgent = rd.vec3d(doc, "agent");
        c.mCamera = rd.vec3d(doc, "camera");
        c.mRange = rd.num(doc, "range");

        Elem pe;
        if (rd.member(doc, "params", pe))
        {
            TileBuildInput& p = c.mParams;
            p.mTileSize = rd.f32(pe, "tile_size");
            p.mCell = rd.f32(pe, "cell");
            p.mPartition = static_cast<Partition>(static_cast<S32>(rd.num(pe, "partition")));
            p.mDetailSampleCells = rd.f32(pe, "detail_sample_cells");
            p.mDetailMaxErrorCells = rd.f32(pe, "detail_max_error_cells");
            p.mWallMinHeight = rd.f32(pe, "wall_min_height");
            p.mWallFaces = rd.flag(pe, "wall_faces");
            p.mDropSpacing = rd.f32(pe, "drop_spacing");
            p.mAir = rd.flag(pe, "air");
            p.mAirPad = static_cast<S32>(rd.num(pe, "air_pad"));
            p.mAirMinGap = rd.f32(pe, "air_min_gap");
            S32 k = 0;
            for (Elem ce : rd.array(pe, "classes"))
            {
                if (k >= NUM_CLASSES)
                {
                    break;
                }
                p.mClasses[k] = rd.flag(ce, "built");
                p.mParams[k].mHeight = rd.f32(ce, "height");
                p.mParams[k].mRadius = rd.f32(ce, "radius");
                p.mParams[k].mClimb = rd.f32(ce, "climb");
                p.mParams[k].mSlope = rd.f32(ce, "slope");
                p.mEdges[k] = rd.flag(ce, "edges");
                p.mDropMax[k] = rd.f32(ce, "drop_max");
                ++k;
            }
        }
        Elem se;
        if (rd.member(doc, "settings", se))
        {
            c.mAirRadius = rd.f32(se, "air_radius");
            c.mBuildWeather = rd.flag(se, "build_weather");
            c.mClusterGap = rd.num(se, "cluster_gap");
            c.mGroundedMargin = rd.f32(se, "grounded_margin");
            c.mVoidMargin = rd.f32(se, "void_margin");
        }
        Elem ee;
        if (rd.member(doc, "enclosure", ee))
        {
            EnclosureParams& e = c.mEnclosure;
            e.mSkyDecayM = rd.f32(ee, "sky_decay");
            e.mDepthFullM = rd.f32(ee, "depth_full");
            e.mClimbM = rd.f32(ee, "climb");
            e.mWalkHeadM = rd.f32(ee, "walk_head");
            e.mWalkOverlapM = rd.f32(ee, "walk_overlap");
            e.mCarryOverlapM = rd.f32(ee, "carry_overlap");
            e.mCarryRiseM = rd.f32(ee, "carry_rise");
            e.mSkyReachM = rd.f32(ee, "sky_reach");
            e.mHysteresis = rd.f32(ee, "hysteresis");
            e.mSealedIndoors = rd.flag(ee, "sealed_indoors");
        }

        for (Elem re : rd.array(doc, "regions"))
        {
            CensusRegion r;
            r.mId = rd.id(re, "id");
            r.mHandle = std::strtoull(rd.text(re, "handle").c_str(), nullptr, 10);
            r.mName = rd.text(re, "name");
            r.mOrigin = rd.vec3d(re, "origin");
            r.mWidth = rd.f32(re, "width");
            r.mWater = rd.f32(re, "water");
            const simdjson::dom::array land = rd.array(re, "land");
            r.mLandMin = static_cast<F32>(rd.at(land, 0));
            r.mLandMax = static_cast<F32>(rd.at(land, 1));
            r.mPhase = rd.f32(re, "phase");
            r.mSamples = static_cast<S32>(rd.num(re, "samples"));
            r.mZ = rd.packed<F32>(re, "z");
            if (r.mZ.size() != static_cast<size_t>(r.mSamples) * r.mSamples)
            {
                rd.fail("terrain samples do not fill the grid");
            }
            c.mRegions.push_back(std::move(r));
        }

        Elem recs;
        if (rd.member(doc, "records", recs))
        {
            const size_t count = static_cast<size_t>(rd.num(recs, "count"));
            const std::string blob = rd.blob(recs, "blob");
            const std::vector<LLVector3> vertices = rd.packed<LLVector3>(recs, "vertices");
            const std::vector<S32> convex = rd.packed<S32>(recs, "convex");
            if (blob.size() != count * RECORD_BYTES)
            {
                rd.fail("records blob size");
            }
            Cursor cur{ reinterpret_cast<const U8*>(blob.data()), blob.size() };
            std::map<U32, std::shared_ptr<const std::vector<LLVector3>>> soups;
            for (size_t i = 0; i < count && rd.mError.empty(); ++i)
            {
                auto r = std::make_shared<Record>();
                for (U8& b : r->mRoot.mData)
                {
                    b = cur.get<U8>();
                }
                for (U8& b : r->mPart.mData)
                {
                    b = cur.get<U8>();
                }
                r->mRole = static_cast<Role>(cur.get<U8>());
                r->mProvenance = static_cast<Provenance>(cur.get<U8>());
                r->mNavRole = static_cast<NavRole>(cur.get<U8>());
                r->mShape = static_cast<Shape>(cur.get<U8>());
                r->mDynamic = cur.get<U8>() != 0;
                r->mSolid = cur.get<U8>() != 0;
                r->mFeature = static_cast<FeatureKind>(cur.get<U8>());
                r->mExclude = cur.get<U8>();
                for (F64& v : r->mOrigin.mdV)
                {
                    v = cur.get<F64>();
                }
                for (F32& v : r->mCenter.mV)
                {
                    v = cur.get<F32>();
                }
                for (LLVector3& a : r->mAxes)
                {
                    for (F32& v : a.mV)
                    {
                        v = cur.get<F32>();
                    }
                }
                for (F32& v : r->mHalf.mV)
                {
                    v = cur.get<F32>();
                }
                for (F64& v : r->mMin.mdV)
                {
                    v = cur.get<F64>();
                }
                for (F64& v : r->mMax.mdV)
                {
                    v = cur.get<F64>();
                }
                r->mSignature = cur.get<U64>();
                const U32 tri_first = cur.get<U32>();
                const U32 tri_count = cur.get<U32>();
                const U32 convex_first = cur.get<U32>();
                const U32 convex_count = cur.get<U32>();
                if (tri_first != NO_TRIANGLES)
                {
                    if (static_cast<size_t>(tri_first) + tri_count > vertices.size())
                    {
                        rd.fail("triangles past the vertices");
                        break;
                    }
                    std::shared_ptr<const std::vector<LLVector3>>& soup = soups[tri_first];
                    if (!soup || soup->size() != tri_count)
                    {
                        soup = std::make_shared<const std::vector<LLVector3>>(vertices.begin() + tri_first, vertices.begin() + tri_first + tri_count);
                    }
                    r->mTriangles = soup;
                }
                if (static_cast<size_t>(convex_first) + convex_count > convex.size())
                {
                    rd.fail("bodies past the convex list");
                    break;
                }
                r->mConvexTris.assign(convex.begin() + convex_first, convex.begin() + convex_first + convex_count);
                c.mRecords.push_back(r);
            }
            if (!cur.ok())
            {
                rd.fail("records blob short");
            }
        }

        for (Elem ce : rd.array(doc, "clusters"))
        {
            CensusCluster cl;
            cl.mId = rd.id(ce, "id");
            cl.mGrounded = rd.flag(ce, "grounded");
            cl.mRecords = rd.packed<U32>(ce, "records");
            for (U32 i : cl.mRecords)
            {
                if (i >= c.mRecords.size())
                {
                    rd.fail("cluster member past the records");
                }
            }
            c.mClusters.push_back(std::move(cl));
        }

        if (version >= 2)
        {
            for (Elem re : rd.array(doc, "roots"))
            {
                CensusRoot r;
                r.mId = rd.id(re, "id");
                r.mName = rd.text(re, "name");
                r.mDescription = rd.text(re, "description");
                Elem scripted;
                if (rd.member(re, "scripted", scripted) && !scripted.is_null())
                {
                    r.mScriptedKnown = true;
                    r.mScripted = rd.flag(re, "scripted");
                }
                const std::string source = rd.text(re, "source");
                r.mSource = source == "linkset" ? NameSource::LINKSET : source == "properties" ? NameSource::PROPERTIES : NameSource::NONE;
                c.mRoots.push_back(std::move(r));
            }
        }

        for (Elem fe : rd.array(doc, "features"))
        {
            Feature f;
            f.mKind = static_cast<FeatureKind>(static_cast<S32>(rd.num(fe, "kind")));
            f.mConfidence = rd.f32(fe, "confidence");
            f.mMin = rd.vec3d(fe, "min");
            f.mMax = rd.vec3d(fe, "max");
            f.mFoot = rd.num(fe, "foot");
            f.mTop = rd.num(fe, "top");
            f.mNormal = rd.vec3(fe, "normal");
            f.mWorking = rd.flag(fe, "working");
            f.mCombat = rd.flag(fe, "combat");
            f.mAir = rd.flag(fe, "air");
            f.mEvidence = rd.text(fe, "evidence");
            for (Elem d : rd.array(fe, "doors"))
            {
                int64_t v = 0;
                if (d.get_int64().get(v) != simdjson::SUCCESS)
                {
                    rd.fail("door index");
                }
                f.mDoors.push_back(static_cast<S32>(v));
            }
            f.mRoots = rd.idList(fe, "roots");
            f.mParts = rd.idList(fe, "parts");
            c.mFeatures.push_back(std::move(f));
        }

        for (Elem de : rd.array(doc, "doors"))
        {
            DoorEntity d;
            d.mId = rd.id(de, "id");
            d.mRegion = std::strtoull(rd.text(de, "region").c_str(), nullptr, 10);
            d.mMin = rd.vec3d(de, "min");
            d.mMax = rd.vec3d(de, "max");
            d.mPlanePoint = rd.vec3d(de, "plane");
            d.mPlaneKnown = rd.flag(de, "planeKnown");
            d.mNormal = rd.vec3(de, "normal");
            d.mWidth = rd.f32(de, "width");
            d.mHeight = rd.f32(de, "height");
            d.mSill = rd.num(de, "sill");
            d.mDouble = rd.flag(de, "double");
            d.mConfidence = rd.f32(de, "confidence");
            d.mDamageable = rd.flag(de, "damageable");
            d.mName = rd.text(de, "name");
            d.mLeaves = rd.idList(de, "leaves");
            c.mDoors.push_back(std::move(d));
        }

        for (Elem sp : rd.array(doc, "spans"))
        {
            CensusSpan s;
            s.mId = rd.id(sp, "id");
            s.mKind = static_cast<LinkKind>(static_cast<S32>(rd.num(sp, "kind")));
            s.mMin = rd.vec3d(sp, "min");
            s.mMax = rd.vec3d(sp, "max");
            S32 k = 0;
            for (Elem ce : rd.array(sp, "classes"))
            {
                if (k >= NUM_CLASSES)
                {
                    break;
                }
                for (Elem x : rd.array(ce, "exits"))
                {
                    s.mExits[k].push_back(rd.vec3d(x));
                }
                for (Elem l : rd.array(ce, "links"))
                {
                    simdjson::dom::array pair;
                    Elem a, b;
                    if (l.get_array().get(pair) != simdjson::SUCCESS || pair.at(0).get(a) != simdjson::SUCCESS ||
                        pair.at(1).get(b) != simdjson::SUCCESS)
                    {
                        rd.fail("span link");
                        break;
                    }
                    s.mLinks[k].emplace_back(rd.vec3d(a), rd.vec3d(b));
                }
                ++k;
            }
            c.mSpans.push_back(std::move(s));
        }

        if (!rd.mError.empty())
        {
            if (error)
            {
                *error = rd.mError;
            }
            return false;
        }
        return true;
    }

    bool readCensusFile(const std::string& path, CensusFile& out, std::string* error)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            if (error)
            {
                *error = "cannot open " + path;
            }
            return false;
        }
        std::ostringstream text;
        text << in.rdbuf();
        return readCensus(text.str(), out, error);
    }

    void censusTerrain(const CensusFile& census, const LLVector3d& mesh_origin, const LLVector3d& tile_min, const LLVector3d& tile_max, F32 pad,
                       TerrainWindow& out)
    {
        // SKNavMesh::fillTerrain, with the regions' samples for resolveHeightGlobal.
        const F32 spacing = 1.f;
        const LLVector3d start(tile_min.mdV[VX] - pad, tile_min.mdV[VY] - pad, 0.0);
        const S32 n = static_cast<S32>(std::ceil((tile_max.mdV[VX] - tile_min.mdV[VX] + 2 * pad) / spacing)) + 1;
        out.mOrigin.set(static_cast<F32>(start.mdV[VX] - mesh_origin.mdV[VX]), static_cast<F32>(start.mdV[VY] - mesh_origin.mdV[VY]), 0.f);
        out.mSpacing = spacing;
        out.mWidth = n;
        out.mHeight = n;
        out.mZ.assign(static_cast<size_t>(n) * n, 0.f);
        const LLVector3d mid = (tile_min + tile_max) * 0.5;
        const CensusRegion* centre = census.regionAt(mid.mdV[VX], mid.mdV[VY]);
        out.mWater = centre ? centre->mWater - static_cast<F32>(mesh_origin.mdV[VZ]) : -1e9f;
        bool any = false;
        for (S32 y = 0; y < n; ++y)
        {
            for (S32 x = 0; x < n; ++x)
            {
                const F64 px = start.mdV[VX] + x * spacing, py = start.mdV[VY] + y * spacing;
                const CensusRegion* region = census.regionAt(px, py);
                F32 z = 0.f;
                if (region && region->heightAt(px, py, z))
                {
                    out.mZ[y * n + x] = z - static_cast<F32>(mesh_origin.mdV[VZ]);
                    any = true;
                }
                else
                {
                    out.mZ[y * n + x] = -1000.f;
                }
            }
        }
        if (!any)
        {
            out.mWidth = out.mHeight = 0;
            out.mZ.clear();
        }
    }
}
