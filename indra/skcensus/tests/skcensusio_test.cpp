/**
 * @file skcensusio_test.cpp
 * @brief Tests for the census export: every field a build reads survives the file.
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

#include "../skcensusio.h"
#include "../skcensusrecord.h"

#include "llbase64.h"
#include "llstring.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <iostream>
#include <sstream>

namespace
{
    using namespace skworld;

    LLUUID id(U32 n)
    {
        LLUUID u;
        u.mData[0] = static_cast<U8>(n);
        u.mData[15] = static_cast<U8>(n >> 8);
        return u;
    }

    /// A census with one of everything, and values no default would give.
    CensusFile sample()
    {
        CensusFile c;
        c.mExported = "2026-09-27T12:00:00Z";
        c.mStamp = 42;
        c.mAgent.set(256128.5, 255900.25, 23.1);
        c.mCamera.set(256120.125, 255890.0, 30.7);
        c.mRange = 192.0;
        c.mClusterGap = 0.45;
        c.mVoidMargin = 128.f;

        CensusRegion r;
        r.mId = id(77);
        r.mHandle = (static_cast<U64>(256000) << 32) | 255744;
        r.mOrigin.set(256000.0, 255744.0, 0.0);
        r.mName = "Test \"Quoted\" Region";
        r.mWater = 20.f;
        r.mLandMin = 19.5f;
        r.mLandMax = 40.25f;
        r.mPhase = 0.625f;
        r.mSamples = 256;
        r.mZ.resize(256 * 256);
        for (size_t i = 0; i < r.mZ.size(); ++i)
        {
            r.mZ[i] = 20.f + 0.001f * static_cast<F32>(i % 997);
        }
        c.mRegions.push_back(r);

        // A box, two parts sharing one soup, and a convex-split soup.
        auto box = std::make_shared<Record>();
        box->mRoot = id(1);
        box->mPart = id(2);
        box->mShape = Shape::BOX;
        box->mNavRole = NavRole::EXCLUSION_VOLUME;
        box->mSolid = true;
        box->mExclude = EXCLUDE_SOLID;
        box->mOrigin = r.mOrigin;
        box->mCenter.set(10.1f, 20.2f, 30.3f);
        box->mAxes[0].set(0.6f, 0.8f, 0.f);
        box->mAxes[1].set(-0.8f, 0.6f, 0.f);
        box->mAxes[2].set(0.f, 0.f, 1.f);
        box->mHalf.set(1.f, 2.f, 3.f);
        box->mMin.set(256008.1, 255761.2, 27.3);
        box->mMax.set(256012.1, 255779.2, 33.3);
        box->mSignature = 0x0123456789abcdefull;
        box->mCollides = false;
        box->mPhysicsKnown = false;
        auto soup = std::make_shared<const std::vector<LLVector3>>(std::vector<LLVector3>{
            LLVector3(1.f, 2.f, 3.f), LLVector3(4.f, 5.f, 6.f), LLVector3(7.f, 8.f, 9.5f) });
        auto a = std::make_shared<Record>();
        a->mRoot = id(3);
        a->mPart = id(4);
        a->mRole = Role::SURROUND;
        a->mProvenance = Provenance::RENDER;
        a->mFeature = FeatureKind::DOOR;
        a->mDynamic = true;
        a->mOrigin = r.mOrigin;
        a->mTriangles = soup;
        a->mMin.set(256001.0, 255746.0, 3.0);
        a->mMax.set(256007.0, 255752.0, 9.5);
        auto b = std::make_shared<Record>(*a);
        b->mPart = id(5);
        b->mProvenance = Provenance::SIM;
        auto hull = std::make_shared<Record>();
        hull->mRoot = id(6);
        hull->mPart = id(6);
        hull->mProvenance = Provenance::HULL;
        hull->mTriangles = std::make_shared<const std::vector<LLVector3>>(std::vector<LLVector3>{
            LLVector3(0.f, 0.f, 0.f), LLVector3(1.f, 0.f, 0.f), LLVector3(0.f, 1.f, 0.f),
            LLVector3(0.f, 0.f, 1.f), LLVector3(1.f, 0.f, 1.f), LLVector3(0.f, 1.f, 1.f) });
        hull->mConvexTris = { 1, 1 };
        c.mRecords = { box, a, b, hull };

        CensusCluster cl;
        cl.mId = id(1);
        cl.mGrounded = true;
        cl.mRecords = { 0, 3 };
        c.mClusters.push_back(cl);

        Feature f;
        f.mKind = FeatureKind::ELEVATOR;
        f.mConfidence = 0.8f;
        f.mParts = { id(4), id(5) };
        f.mRoots = { id(3) };
        f.mMin.set(1.0, 2.0, 3.0);
        f.mMax.set(4.0, 5.0, 60.0);
        f.mFoot = 3.0;
        f.mTop = 60.0;
        f.mNormal.set(1.f, 0.f, 0.f);
        f.mDoors = { 2, 7 };
        f.mWorking = true;
        f.mEvidence = "car \"in\" column";
        c.mFeatures.push_back(f);

        DoorEntity d;
        d.mId = id(4);
        d.mRegion = r.mHandle;
        d.mMin.set(256001.0, 255746.0, 3.0);
        d.mMax.set(256002.0, 255748.0, 5.5);
        d.mPlanePoint.set(256001.5, 255747.0, 3.0);
        d.mPlaneKnown = false;
        d.mNormal.set(1.f, 0.f, 0.f);
        d.mWidth = 2.f;
        d.mHeight = 2.5f;
        d.mSill = 3.0;
        d.mDouble = true;
        d.mLeaves = { id(4), id(5) };
        d.mConfidence = 0.7f;
        d.mDamageable = true;
        d.mName = "Front\\door";
        c.mDoors.push_back(d);

        return c;
    }

    bool sameRecord(const Record& a, const Record& b)
    {
        if (!(a.mRoot == b.mRoot && a.mPart == b.mPart && a.mRole == b.mRole && a.mProvenance == b.mProvenance && a.mNavRole == b.mNavRole &&
              a.mShape == b.mShape && a.mDynamic == b.mDynamic && a.mSolid == b.mSolid && a.mFeature == b.mFeature && a.mExclude == b.mExclude &&
              a.mCollides == b.mCollides && a.mPhysicsKnown == b.mPhysicsKnown &&
              a.mOrigin == b.mOrigin && a.mCenter == b.mCenter && a.mHalf == b.mHalf && a.mMin == b.mMin && a.mMax == b.mMax &&
              a.mSignature == b.mSignature && a.mConvexTris == b.mConvexTris))
        {
            return false;
        }
        for (S32 k = 0; k < 3; ++k)
        {
            if (a.mAxes[k] != b.mAxes[k])
            {
                return false;
            }
        }
        if (!a.mTriangles || !b.mTriangles)
        {
            return !a.mTriangles && !b.mTriangles;
        }
        return *a.mTriangles == *b.mTriangles;
    }

    CensusFile roundTrip(const CensusFile& c)
    {
        std::ostringstream out;
        writeCensus(out, c);
        CensusFile back;
        std::string error;
        INFO(error);
        REQUIRE(readCensus(out.str(), back, &error));
        return back;
    }

    std::string currentVersion()
    {
        return "\"version\":" + std::to_string(CENSUS_FORMAT_VERSION);
    }

}

namespace
{
    TEST_CASE("skcensusio: every record field and setting survives the file", "[skcensusio]")
    {
        const CensusFile c = sample();
        const CensusFile back = roundTrip(c);
        CHECK(back.mRecords.size() == c.mRecords.size());
        for (size_t i = 0; i < c.mRecords.size(); ++i)
        {
            INFO("record " + std::to_string(i));
            CHECK(sameRecord(*c.mRecords[i], *back.mRecords[i]));
            INFO("content key " + std::to_string(i));
            CHECK(recordContentKey(*back.mRecords[i]) == recordContentKey(*c.mRecords[i]));
        }
        CHECK(back.mRecords[1]->mTriangles == back.mRecords[2]->mTriangles);
        CHECK(back.mClusterGap == c.mClusterGap);
        CHECK(back.mVoidMargin == c.mVoidMargin);
        CHECK(back.mCamera == c.mCamera);
        CHECK(back.mStamp == c.mStamp);
    }

    TEST_CASE("skcensusio: regions clusters features and doors survive the file", "[skcensusio]")
    {
        const CensusFile c = sample();
        const CensusFile back = roundTrip(c);
        CHECK(back.mRegions.size() == 1u);
        const CensusRegion& r = back.mRegions[0];
        CHECK(r.mId == c.mRegions[0].mId);
        CHECK(r.mHandle == c.mRegions[0].mHandle);
        CHECK(r.mName == c.mRegions[0].mName);
        CHECK(r.mZ == c.mRegions[0].mZ);
        CHECK(r.mPhase == 0.625f);
        CHECK(r.mLandMax == 40.25f);

        CHECK(back.mClusters.size() == 1u);
        CHECK((back.mClusters[0].mRecords == c.mClusters[0].mRecords && back.mClusters[0].mGrounded));

        CHECK(back.mFeatures.size() == 1u);
        const Feature& f = back.mFeatures[0];
        CHECK((f.mKind == FeatureKind::ELEVATOR && f.mParts == c.mFeatures[0].mParts && f.mRoots == c.mFeatures[0].mRoots && f.mDoors == c.mFeatures[0].mDoors && f.mWorking && f.mEvidence == c.mFeatures[0].mEvidence && f.mTop == 60.0));

        CHECK(back.mDoors.size() == 1u);
        const DoorEntity& d = back.mDoors[0];
        const DoorEntity& o = c.mDoors[0];
        CHECK((d.mId == o.mId && d.mRegion == o.mRegion && d.mMin == o.mMin && d.mMax == o.mMax && d.mPlanePoint == o.mPlanePoint && d.mPlaneKnown == o.mPlaneKnown && d.mNormal == o.mNormal && d.mWidth == o.mWidth && d.mHeight == o.mHeight && d.mSill == o.mSill && d.mDouble == o.mDouble && d.mLeaves == o.mLeaves && d.mConfidence == o.mConfidence && d.mDamageable == o.mDamageable && d.mName == o.mName));
    }

    TEST_CASE("skcensusio: a terrain window reads the region's samples where the viewer would and the void past it", "[skcensusio]")
    {
        CensusFile c = sample();
        const CensusRegion& r = c.mRegions[0];
        // Pad 2.375 puts the window's samples at 0.625 past each metre, the region's phase.
        const LLVector3d mesh_origin(256000.0, 255744.0, 0.0);
        const LLVector3d tmin(256000.0, 255744.0, 0.0), tmax(256032.0, 255776.0, 0.0);
        TerrainWindow t;
        censusTerrain(c, mesh_origin, tmin, tmax, 2.375f, t);
        CHECK(t.mWidth == 38);
        CHECK(t.mOrigin.mV[VX] == -2.375f);
        // Sample (3, 4) is at region-local (0.625, 1.625): the region's (0, 1).
        CHECK(t.mZ[4 * 38 + 3] == r.mZ[1 * 256 + 0]);
        CHECK(t.mZ[0] == -1000.f);
        CHECK(t.mWater == 20.f);
        TerrainWindow off;
        censusTerrain(c, mesh_origin, tmin - LLVector3d(512.0, 0.0, 0.0), tmax - LLVector3d(512.0, 0.0, 0.0), 2.375f, off);
        CHECK((off.mWidth == 0 && off.mZ.empty()));
    }

    TEST_CASE("skcensusio: a file of another format or a damaged blob is refused", "[skcensusio]")
    {
        std::ostringstream out;
        writeCensus(out, sample());
        std::string text = out.str();
        CensusFile back;
        std::string error;
        std::string other = text;
        other.replace(other.find("sknav-census"), 12, "sknav-export");
        CHECK(!readCensus(other, back, &error));
        std::string cut = text;
        const size_t blob = cut.find("\"blob\":\"") + 8;
        cut.erase(blob, 8);
        CHECK(!readCensus(cut, back, &error));
    }

    TEST_CASE("skcensusio: what writing a large census costs when asked", "[skcensusio][.]")
    {
        if (LLStringUtil::getenv("SKCENSUS_IO_BENCH").empty())
        {
            return;
        }
        // 25 regions of land, 20000 records, half of them 200-vertex soups.
        CensusFile c = sample();
        const CensusRegion region = c.mRegions[0];
        c.mRegions.assign(25, region);
        const RecordPtr box = c.mRecords[0], soup = c.mRecords[3];
        c.mRecords.clear();
        for (S32 i = 0; i < 20000; ++i)
        {
            auto r = std::make_shared<Record>(i & 1 ? *soup : *box);
            if (i & 1)
            {
                std::vector<LLVector3> v(200);
                for (S32 k = 0; k < 200; ++k)
                {
                    v[k].set(static_cast<F32>(i % 256), static_cast<F32>(k), 0.5f * k);
                }
                r->mTriangles = std::make_shared<const std::vector<LLVector3>>(std::move(v));
                r->mConvexTris.clear();
            }
            c.mRecords.push_back(r);
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::ostringstream out;
        writeCensus(out, c);
        const F64 ms = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "writeCensus: " << ms << " ms for " << out.str().size() / (1024.0 * 1024.0) << " MB" << std::endl;
    }

    TEST_CASE("skcensusio: the root table survives the file descriptions are capped and clusters are labelled by their roots", "[skcensusio]")
    {
        CensusFile c = sample();
        CensusRoot hotel;
        hotel.mId = id(1);
        hotel.mName = "Hotel \"lobby\"\n";
        hotel.mDescription = "short";
        hotel.mScriptedKnown = true;
        hotel.mScripted = true;
        hotel.mSource = NameSource::LINKSET;
        CensusRoot ship;
        ship.mId = id(3);
        ship.mName = "Ship";
        // 200 two-byte characters: 400 bytes, cut to 256 on a boundary.
        for (S32 k = 0; k < 200; ++k)
        {
            ship.mDescription += "\xc3\xa9";
        }
        ship.mSource = NameSource::PROPERTIES;
        CensusRoot unknown;
        unknown.mId = id(6);
        c.mRoots = { hotel, ship, unknown };
        CensusCluster all;
        all.mId = id(9);
        all.mRecords = { 0, 1, 2, 3 };
        c.mClusters.push_back(all);

        CHECK(clusterLabel(c, c.mClusters[1]) == std::string("Ship (+2)"));
        CHECK(clusterLabel(c, c.mClusters[0]) == std::string("Hotel \"lobby\"\n (+1)"));
        const std::vector<RootShare> top = dominantRoots(c, c.mClusters[1], 2);
        CHECK((top.size() == 2 && top[0].mId == id(3) && top[0].mParts == 2 && top[1].mId == id(1)));

        std::ostringstream out;
        writeCensus(out, c);
        const std::string text = out.str();
        CHECK(text.find("\"label\":\"Ship (+2)\",\"top_roots\":[{\"id\":\"" + id(3).asString() + "\",\"parts\":2,\"name\":\"Ship\"}") != std::string::npos);
        CensusFile back;
        std::string error;
        INFO("reads: " + error);
        CHECK(readCensus(text, back, &error));
        CHECK(back.mRoots.size() == 3u);
        const CensusRoot& h = back.mRoots[0];
        CHECK((h.mId == hotel.mId && h.mName == hotel.mName && h.mDescription == "short" && h.mScriptedKnown && h.mScripted && h.mSource == NameSource::LINKSET));
        const CensusRoot& s = back.mRoots[1];
        CHECK(s.mDescription.size() == ROOT_DESCRIPTION_MAX);
        CHECK((s.mName == "Ship" && s.mDescription == ship.mDescription.substr(0, 256) && !s.mScriptedKnown && s.mSource == NameSource::PROPERTIES));
        CHECK((back.mRoots[2].mId == id(6) && back.mRoots[2].mSource == NameSource::NONE && !back.mRoots[2].mScriptedKnown));
        CHECK(capDescription(std::string(255, 'a') + "\xc3\xa9").size() == 255u);

        // A version 1 file has no root table and still reads.
        std::string old = text;
        old.replace(old.find(currentVersion()), 11, "\"version\":1");
        old.erase(old.find(",\"roots\":"), old.find(",\"features\":") - old.find(",\"roots\":"));
        INFO("version 1 reads: " + error);
        CHECK(readCensus(old, back, &error));
        CHECK((back.mRoots.empty() && back.mRecords.size() == c.mRecords.size()));
    }

    TEST_CASE("skcensusio: a record from the simulator's triangles says so and an older file still reads", "[skcensusio]")
    {
        const CensusFile c = sample();
        REQUIRE(c.mRecords[2]->mProvenance == Provenance::SIM);
        std::ostringstream out;
        writeCensus(out, c);
        const std::string text = out.str();
        CHECK(CENSUS_FORMAT_VERSION == 7u);
        CHECK(text.find(currentVersion()) != std::string::npos);
        CensusFile back;
        std::string error;
        REQUIRE(readCensus(text, back, &error));
        REQUIRE(back.mRecords.size() == c.mRecords.size());
        CHECK(back.mRecords[2]->mProvenance == Provenance::SIM);
        CHECK(back.mRecords[1]->mProvenance == Provenance::RENDER);

        std::string old = text;
        old.replace(old.find(currentVersion()), 11, "\"version\":4");
        INFO(error);
        CHECK(readCensus(old, back, &error));

        std::string future = text;
        future.replace(future.find(currentVersion()), 11, "\"version\":8");
        CHECK_FALSE(readCensus(future, back, &error));
    }

    TEST_CASE("skcensusio: collides and physics known round-trip and a version five file reads as colliding", "[skcensusio]")
    {
        CensusFile c = sample();
        REQUIRE(c.mRecords.size() >= 3u);
        REQUIRE((!c.mRecords[0]->mCollides && !c.mRecords[0]->mPhysicsKnown));
        const CensusFile back = roundTrip(c);
        REQUIRE(back.mRecords.size() == c.mRecords.size());
        CHECK((!back.mRecords[0]->mCollides && !back.mRecords[0]->mPhysicsKnown));
        CHECK((back.mRecords[1]->mCollides && back.mRecords[1]->mPhysicsKnown));

        // A version 5 file: the same records without the two bytes after exclude.
        std::ostringstream out;
        writeCensus(out, c);
        std::string text = out.str();
        const size_t at = text.find("\"blob\":\"", text.find("\"records\":{")) + 8;
        const size_t end = text.find('"', at);
        const std::string blob = LLBase64::decodeAsString(text.substr(at, end - at));
        REQUIRE(blob.size() == c.mRecords.size() * 198);
        std::string old_blob;
        for (size_t i = 0; i < c.mRecords.size(); ++i)
        {
            const std::string rec = blob.substr(i * 198, 198);
            old_blob += rec.substr(0, 40) + rec.substr(42);
        }
        text.replace(at, end - at, LLBase64::encode(reinterpret_cast<const U8*>(old_blob.data()), old_blob.size()));
        text.replace(text.find("\"record_bytes\":198"), 18, "\"record_bytes\":196");
        text.replace(text.find(currentVersion()), 11, "\"version\":5");
        CensusFile old;
        std::string error;
        INFO(error);
        REQUIRE(readCensus(text, old, &error));
        REQUIRE(old.mRecords.size() == c.mRecords.size());
        for (size_t i = 0; i < c.mRecords.size(); ++i)
        {
            INFO("record " + std::to_string(i));
            CHECK((old.mRecords[i]->mCollides && old.mRecords[i]->mPhysicsKnown));
            Record expect = *c.mRecords[i];
            expect.mCollides = expect.mPhysicsKnown = true;
            CHECK(sameRecord(expect, *old.mRecords[i]));
        }
    }

    TEST_CASE("skcensusio: a layer's sections round-trip by name and an unknown one is skipped", "[skcensusio]")
    {
        const CensusFile c = sample();
        LLSD air_back, rooms_back;
        U32 air_version = 0;
        const CensusSections write = {
            { "air.params", [](std::ostream& out) { out << "{\"cell\":0.125,\"pad\":16,\"air\":true,\"name\":\"a \\\"b\\\"\"}"; }, nullptr },
            { "rooms.enclosure", [](std::ostream& out) { out << "[1,2.5,{\"deep\":[null]}]"; }, nullptr },
            { "someone.else", [](std::ostream& out) { out << "{}"; }, nullptr },
        };
        std::ostringstream out;
        writeCensus(out, c, write);
        const std::string text = out.str();
        CHECK(text.find("\"sections\":{\"air.params\":") != std::string::npos);

        const CensusSections read = {
            { "air.params", nullptr, [&](const LLSD& v, U32 version) { air_back = v; air_version = version; return true; } },
            { "rooms.enclosure", nullptr, [&](const LLSD& v, U32) { rooms_back = v; return true; } },
        };
        CensusFile back;
        std::string error;
        REQUIRE(readCensus(text, back, &error, read));
        CHECK(air_version == CENSUS_FORMAT_VERSION);
        CHECK(air_back["cell"].asReal() == 0.125);
        CHECK(air_back["pad"].asInteger() == 16);
        CHECK(air_back["air"].asBoolean());
        CHECK(air_back["name"].asString() == "a \"b\"");
        REQUIRE(rooms_back.isArray());
        CHECK(rooms_back.size() == 3);
        CHECK(rooms_back[1].asReal() == 2.5);
        CHECK(rooms_back[2]["deep"][0].isUndefined());
        CHECK(back.mRecords.size() == c.mRecords.size());

        // A reader that refuses its value fails the read and names the section.
        const CensusSections refuse = { { "air.params", nullptr, [](const LLSD&, U32) { return false; } } };
        CHECK_FALSE(readCensus(text, back, &error, refuse));
        CHECK(error == "section air.params");

        // Read with no readers, the census alone.
        CHECK(readCensus(text, back, &error));
    }

    TEST_CASE("skcensusio: an older file's top-level parts are handed to their sections by today's names", "[skcensusio]")
    {
        std::ostringstream out;
        writeCensus(out, sample());
        std::string text = out.str();
        text.replace(text.find(currentVersion()), 11, "\"version\":6");
        text.erase(text.find(",\"sections\":{"));
        text += ",\"params\":{\"cell\":0.25},\"enclosure\":{\"climb\":0.5},\"probes\":[{\"kind\":\"octree\"}]}\n";
        const size_t records = text.find("\"records\":{");
        text.insert(text.find('}', text.find("\"convex\":", records)), ",\"seen\":\"AAE=\"");
        LLSD params, enclosure, probes, seen;
        U32 params_version = 0;
        const CensusSections read = {
            { "air.params", nullptr, [&](const LLSD& v, U32 version) { params = v; params_version = version; return true; } },
            { "rooms.enclosure", nullptr, [&](const LLSD& v, U32) { enclosure = v; return true; } },
            { "probes", nullptr, [&](const LLSD& v, U32) { probes = v; return true; } },
            { "overview.seen", nullptr, [&](const LLSD& v, U32) { seen = v; return true; } },
        };
        CensusFile back;
        std::string error;
        INFO(error);
        REQUIRE(readCensus(text, back, &error, read));
        CHECK(params_version == 6u);
        CHECK(params["cell"].asReal() == 0.25);
        CHECK(enclosure["climb"].asReal() == 0.5);
        CHECK(probes[0]["kind"].asString() == "octree");
        CHECK(seen.asString() == "AAE=");
    }

    TEST_CASE("skcensusio: json escaping covers quotes backslashes and control characters", "[skcensusio]")
    {
        CHECK(jsonEscape("a\"b\\c\nd\te\x01") == "a\\\"b\\\\c\\nd\\te\\u0001");
    }
}
