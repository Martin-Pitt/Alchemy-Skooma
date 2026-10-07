/**
 * @file sknavcensusio_test.cpp
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

#include "../test/lltut.h"

#include "../sknavcensusio.h"
#include "../sknavtilecache.h"

#include "llstring.h"

#include <chrono>
#include <iostream>
#include <sstream>

namespace
{
    using namespace sknav;

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
        c.mParams.mCell = 0.125f;
        c.mParams.mPartition = Partition::MONOTONE;
        c.mParams.mDetailSampleCells = 5.5f;
        c.mParams.mDetailMaxErrorCells = 1.75f;
        c.mParams.mWallMinHeight = 0.4f;
        c.mParams.mWallFaces = true;
        c.mParams.mDropSpacing = 1.5f;
        c.mParams.mAir = true;
        c.mParams.mAirPad = 12;
        c.mParams.mAirMinGap = 0.375f;
        c.mParams.mClasses[1] = true;
        c.mParams.mParams[0] = { 1.875f, 0.3f, 0.75f, 74.f };
        c.mParams.mParams[1] = { 0.5f, 0.125f, 0.7f, 89.f };
        c.mParams.mEdges[0] = true;
        c.mParams.mDropMax[1] = 20.f;
        c.mAirRadius = 96.f;
        c.mBuildWeather = true;
        c.mClusterGap = 0.45;
        c.mGroundedMargin = 4.f;
        c.mVoidMargin = 128.f;
        c.mEnclosure.mWalkHeadM = 1.875f;
        c.mEnclosure.mSealedIndoors = false;

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

        CensusSpan s;
        s.mId = id(3);
        s.mKind = LinkKind::ELEVATOR;
        s.mMin.set(1.0, 2.0, 3.0);
        s.mMax.set(4.0, 5.0, 60.0);
        s.mExits[0] = { LLVector3d(2.0, 3.0, 3.0), LLVector3d(2.0, 3.0, 30.0) };
        s.mLinks[0] = { { LLVector3d(2.0, 3.0, 3.0), LLVector3d(2.0, 3.0, 30.0) } };
        c.mSpans.push_back(s);
        return c;
    }

    bool sameRecord(const Record& a, const Record& b)
    {
        if (!(a.mRoot == b.mRoot && a.mPart == b.mPart && a.mRole == b.mRole && a.mProvenance == b.mProvenance && a.mNavRole == b.mNavRole &&
              a.mShape == b.mShape && a.mDynamic == b.mDynamic && a.mSolid == b.mSolid && a.mFeature == b.mFeature && a.mExclude == b.mExclude &&
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
        tut::ensure("read back: " + error, readCensus(out.str(), back, &error));
        return back;
    }

    /// A tile's input from the census's params and every record, as a build would take it.
    TileBuildInput asInput(const CensusFile& c)
    {
        TileBuildInput in = c.mParams;
        in.mRecords = c.mRecords;
        return in;
    }
}

namespace tut
{
    struct sknavcensusio_data {};
    typedef test_group<sknavcensusio_data> sknavcensusio_group;
    typedef sknavcensusio_group::object sknavcensusio_object;
    tut::sknavcensusio_group g("sknavcensusio");

    template<> template<>
    void sknavcensusio_object::test<1>()
    {
        set_test_name("every record field and every build parameter survives the file");
        const CensusFile c = sample();
        const CensusFile back = roundTrip(c);
        ensure_equals("records", back.mRecords.size(), c.mRecords.size());
        for (size_t i = 0; i < c.mRecords.size(); ++i)
        {
            ensure("record " + std::to_string(i), sameRecord(*c.mRecords[i], *back.mRecords[i]));
            ensure_equals("content key " + std::to_string(i), recordContentKey(*back.mRecords[i]), recordContentKey(*c.mRecords[i]));
        }
        ensure("a shared soup stays shared", back.mRecords[1]->mTriangles == back.mRecords[2]->mTriangles);
        // The tile key covers every parameter a build reads: equal keys, equal builds.
        ensure_equals("tile key", tileContentKey(asInput(back)), tileContentKey(asInput(c)));
        ensure_equals("walk head", back.mEnclosure.mWalkHeadM, c.mEnclosure.mWalkHeadM);
        ensure_equals("sealed", back.mEnclosure.mSealedIndoors, c.mEnclosure.mSealedIndoors);
        ensure_equals("gap", back.mClusterGap, c.mClusterGap);
        ensure_equals("air radius", back.mAirRadius, c.mAirRadius);
        ensure("build weather", back.mBuildWeather);
        ensure_equals("void margin", back.mVoidMargin, c.mVoidMargin);
        ensure("camera", back.mCamera == c.mCamera);
        ensure_equals("stamp", back.mStamp, c.mStamp);
    }

    template<> template<>
    void sknavcensusio_object::test<2>()
    {
        set_test_name("regions, clusters, features, doors and spans survive the file");
        const CensusFile c = sample();
        const CensusFile back = roundTrip(c);
        ensure_equals("regions", back.mRegions.size(), 1u);
        const CensusRegion& r = back.mRegions[0];
        ensure("id", r.mId == c.mRegions[0].mId);
        ensure_equals("handle", r.mHandle, c.mRegions[0].mHandle);
        ensure_equals("name", r.mName, c.mRegions[0].mName);
        ensure("samples", r.mZ == c.mRegions[0].mZ);
        ensure_equals("phase", r.mPhase, 0.625f);
        ensure_equals("land", r.mLandMax, 40.25f);

        ensure_equals("clusters", back.mClusters.size(), 1u);
        ensure("members", back.mClusters[0].mRecords == c.mClusters[0].mRecords && back.mClusters[0].mGrounded);

        ensure_equals("features", back.mFeatures.size(), 1u);
        const Feature& f = back.mFeatures[0];
        ensure("feature", f.mKind == FeatureKind::ELEVATOR && f.mParts == c.mFeatures[0].mParts && f.mRoots == c.mFeatures[0].mRoots &&
                              f.mDoors == c.mFeatures[0].mDoors && f.mWorking && f.mEvidence == c.mFeatures[0].mEvidence && f.mTop == 60.0);

        ensure_equals("doors", back.mDoors.size(), 1u);
        const DoorEntity& d = back.mDoors[0];
        const DoorEntity& o = c.mDoors[0];
        ensure("door", d.mId == o.mId && d.mRegion == o.mRegion && d.mMin == o.mMin && d.mMax == o.mMax && d.mPlanePoint == o.mPlanePoint &&
                           d.mPlaneKnown == o.mPlaneKnown && d.mNormal == o.mNormal && d.mWidth == o.mWidth && d.mHeight == o.mHeight &&
                           d.mSill == o.mSill && d.mDouble == o.mDouble && d.mLeaves == o.mLeaves && d.mConfidence == o.mConfidence &&
                           d.mDamageable == o.mDamageable && d.mName == o.mName);

        ensure_equals("spans", back.mSpans.size(), 1u);
        const CensusSpan& s = back.mSpans[0];
        ensure("span", s.mId == c.mSpans[0].mId && s.mKind == LinkKind::ELEVATOR && s.mExits[0] == c.mSpans[0].mExits[0] &&
                           s.mLinks[0] == c.mSpans[0].mLinks[0] && s.mExits[1].empty());
    }

    template<> template<>
    void sknavcensusio_object::test<3>()
    {
        set_test_name("a terrain window reads the region's samples where the viewer would, and the void past it");
        CensusFile c = sample();
        const CensusRegion& r = c.mRegions[0];
        // Pad 2.375 puts the window's samples at 0.625 past each metre, the region's phase.
        const LLVector3d mesh_origin(256000.0, 255744.0, 0.0);
        const LLVector3d tmin(256000.0, 255744.0, 0.0), tmax(256032.0, 255776.0, 0.0);
        TerrainWindow t;
        censusTerrain(c, mesh_origin, tmin, tmax, 2.375f, t);
        ensure_equals("width", t.mWidth, 38);
        ensure_equals("origin", t.mOrigin.mV[VX], -2.375f);
        // Sample (3, 4) is at region-local (0.625, 1.625): the region's (0, 1).
        ensure_equals("inside", t.mZ[4 * 38 + 3], r.mZ[1 * 256 + 0]);
        ensure_equals("past the edge is the void", t.mZ[0], -1000.f);
        ensure_equals("water", t.mWater, 20.f);
        TerrainWindow off;
        censusTerrain(c, mesh_origin, tmin - LLVector3d(512.0, 0.0, 0.0), tmax - LLVector3d(512.0, 0.0, 0.0), 2.375f, off);
        ensure("no region, no window", off.mWidth == 0 && off.mZ.empty());
    }

    template<> template<>
    void sknavcensusio_object::test<4>()
    {
        set_test_name("a file of another format or a damaged blob is refused");
        std::ostringstream out;
        writeCensus(out, sample());
        std::string text = out.str();
        CensusFile back;
        std::string error;
        std::string other = text;
        other.replace(other.find("sknav-census"), 12, "sknav-export");
        ensure("another format", !readCensus(other, back, &error));
        std::string cut = text;
        const size_t blob = cut.find("\"blob\":\"") + 8;
        cut.erase(blob, 8);
        ensure("a short records blob", !readCensus(cut, back, &error));
    }

    template<> template<>
    void sknavcensusio_object::test<5>()
    {
        set_test_name("what writing a large census costs, when asked");
        if (LLStringUtil::getenv("SKNAV_CENSUS_BENCH").empty())
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

    template<> template<>
    void sknavcensusio_object::test<6>()
    {
        set_test_name("the session's marks are written with their region, and the file still reads");
        CensusFile c = sample();
        Mark m;
        m.mId = 7;
        m.mType = MarkType::SPLIT;
        m.mVerts.push_back(LLVector3d(256100.5, 255800.25, 25.0));
        c.mMarks.push_back(m);
        std::ostringstream out;
        writeCensus(out, c);
        const std::string text = out.str();
        ensure("marks block", text.find("\"marks\":[{\"id\":7,\"kind\":\"point\",\"type\":\"split\"") != std::string::npos);
        ensure("located", text.find("\"region\":\"Test \\\"Quoted\\\" Region\",\"regionGrid\":[1000,999],\"local\":[100.5,56.25,25]") !=
                              std::string::npos);
        CensusFile back;
        std::string error;
        ensure("reads: " + error, readCensus(text, back, &error));
    }

    template<> template<>
    void sknavcensusio_object::test<7>()
    {
        set_test_name("the root table survives the file, descriptions are capped, and clusters are labelled by their roots");
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

        ensure_equals("label", clusterLabel(c, c.mClusters[1]), std::string("Ship (+2)"));
        ensure_equals("single root", clusterLabel(c, c.mClusters[0]), std::string("Hotel \"lobby\"\n (+1)"));
        const std::vector<RootShare> top = dominantRoots(c, c.mClusters[1], 2);
        ensure("dominant", top.size() == 2 && top[0].mId == id(3) && top[0].mParts == 2 && top[1].mId == id(1));

        std::ostringstream out;
        writeCensus(out, c);
        const std::string text = out.str();
        ensure("label written", text.find("\"label\":\"Ship (+2)\",\"top_roots\":[{\"id\":\"" + id(3).asString() + "\",\"parts\":2,\"name\":\"Ship\"}") !=
                                    std::string::npos);
        CensusFile back;
        std::string error;
        ensure("reads: " + error, readCensus(text, back, &error));
        ensure_equals("roots", back.mRoots.size(), 3u);
        const CensusRoot& h = back.mRoots[0];
        ensure("hotel", h.mId == hotel.mId && h.mName == hotel.mName && h.mDescription == "short" && h.mScriptedKnown && h.mScripted &&
                            h.mSource == NameSource::LINKSET);
        const CensusRoot& s = back.mRoots[1];
        ensure_equals("capped", s.mDescription.size(), ROOT_DESCRIPTION_MAX);
        ensure("ship", s.mName == "Ship" && s.mDescription == ship.mDescription.substr(0, 256) && !s.mScriptedKnown &&
                           s.mSource == NameSource::PROPERTIES);
        ensure("unknown", back.mRoots[2].mId == id(6) && back.mRoots[2].mSource == NameSource::NONE && !back.mRoots[2].mScriptedKnown);
        ensure_equals("odd cut", capDescription(std::string(255, 'a') + "\xc3\xa9").size(), 255u);

        // A version 1 file has no root table and still reads.
        std::string old = text;
        old.replace(old.find("\"version\":3"), 11, "\"version\":1");
        old.erase(old.find(",\"roots\":"), old.find(",\"features\":") - old.find(",\"roots\":"));
        ensure("version 1 reads: " + error, readCensus(old, back, &error));
        ensure("no roots", back.mRoots.empty() && back.mRecords.size() == c.mRecords.size());
    }

    template<> template<>
    void sknavcensusio_object::test<8>()
    {
        set_test_name("every probe kind survives the file, boxes with their rotation and scale, and a version 2 file reads without probes");
        CensusFile c = sample();
        for (U8 k = 0; k < static_cast<U8>(ProbeKind::COUNT); ++k)
        {
            CensusProbe p;
            p.mKind = static_cast<ProbeKind>(k);
            p.mObject = id(20 + k);
            p.mOrigin.set(256000.125 + k, 255900.5, 31.25 + k);
            p.mRadius = 3.5f + k;
            p.mPriority = k == 1 || k == 2 ? 1 : 0;
            p.mRelevant = k != 3;
            p.mSuppressed = k == 3;
            p.mInsideManual = k == 4;
            p.mResident = k % 2 == 0;
            p.mRotation = LLQuaternion(0.5f + k, LLVector3(0.f, 0.6f, 0.8f));
            p.mScale.set(4.f, 2.5f + k, 3.f);
            c.mProbes.push_back(p);
        }

        std::ostringstream out;
        writeCensus(out, c);
        const std::string text = out.str();
        CensusFile back;
        std::string error;
        ensure("reads: " + error, readCensus(text, back, &error));
        ensure_equals("probes", back.mProbes.size(), c.mProbes.size());
        for (size_t i = 0; i < c.mProbes.size(); ++i)
        {
            const CensusProbe& a = c.mProbes[i];
            const CensusProbe& b = back.mProbes[i];
            const std::string what = probeKindName(a.mKind);
            ensure(what, b.mKind == a.mKind && b.mObject == a.mObject && b.mOrigin == a.mOrigin && b.mRadius == a.mRadius &&
                             b.mPriority == a.mPriority && b.mRelevant == a.mRelevant && b.mSuppressed == a.mSuppressed &&
                             b.mInsideManual == a.mInsideManual && b.mResident == a.mResident);
            const bool box = a.mKind == ProbeKind::PLACED_BOX || a.mKind == ProbeKind::NAV_BOX;
            if (box)
            {
                ensure(what + " box", b.mRotation == a.mRotation && b.mScale == a.mScale);
            }
            else
            {
                ensure(what + " no box", b.mRotation == LLQuaternion::DEFAULT && b.mScale == LLVector3::zero);
            }
        }

        // A version 2 file has no probes and still reads.
        std::string old = text;
        old.replace(old.find("\"version\":3"), 11, "\"version\":2");
        old.erase(old.find(",\"probes\":"), old.find(",\"features\":") - old.find(",\"probes\":"));
        ensure("version 2 reads: " + error, readCensus(old, back, &error));
        ensure("no probes", back.mProbes.empty() && back.mRecords.size() == c.mRecords.size());
    }
}
