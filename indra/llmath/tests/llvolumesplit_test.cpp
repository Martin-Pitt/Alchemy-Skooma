/**
 * @file   llvolumesplit_test.cpp
 * @brief  Untwisted flat-sided prims without profile splits match the split geometry
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "../test/lltut.h"

#include "../llvolume.h"
#include "../v3dmath.h"

#include <sstream>
#include <vector>

namespace
{
    struct Tri
    {
        LLVector3 p[3];
        LLVector3 n[3];
        LLVector2 uv[3];
    };

    LLVector3 v3(const LLVector4a& v) { return LLVector3(v.getF32ptr()); }

    std::vector<Tri> triangles(const LLVolumeFace& face)
    {
        std::vector<Tri> tris;
        for (S32 i = 0; i + 2 < face.mNumIndices; i += 3)
        {
            Tri t;
            for (S32 k = 0; k < 3; ++k)
            {
                U16 idx = face.mIndices[i + k];
                t.p[k] = v3(face.mPositions[idx]);
                t.n[k] = v3(face.mNormals[idx]);
                t.uv[k] = face.mTexCoords[idx];
            }
            tris.push_back(t);
        }
        return tris;
    }

    F32 area(const std::vector<Tri>& tris)
    {
        F32 a = 0.f;
        for (const Tri& t : tris)
        {
            a += 0.5f * ((t.p[1] - t.p[0]) % (t.p[2] - t.p[0])).length();
        }
        return a;
    }

    // Barycentric weights of the point on the triangle closest to p, and its distance.
    bool project(const Tri& t, const LLVector3& p, F32& dist, F32 w[3])
    {
        // In doubles: long slivers cancel too much in floats.
        LLVector3d p0(t.p[0]), e0 = LLVector3d(t.p[1]) - p0, e1 = LLVector3d(t.p[2]) - p0, d = LLVector3d(p) - p0;
        F64 a = e0 * e0, b = e0 * e1, c = e1 * e1, x = d * e0, y = d * e1;
        F64 den = a * c - b * b;
        if (den <= 1e-12 * a * c)
        {
            // A sliver: only its corners count.
            for (S32 k = 0; k < 3; ++k)
            {
                if ((t.p[k] - p).length() < 1e-5f)
                {
                    w[0] = w[1] = w[2] = 0.f;
                    w[k] = 1.f;
                    dist = 0.f;
                    return true;
                }
            }
            return false;
        }
        F64 v = (c * x - b * y) / den, u = (a * y - b * x) / den;
        const F64 slack = 1e-4;
        if (v < -slack || u < -slack || v + u > 1.0 + slack)
        {
            return false;
        }
        w[0] = (F32)(1.0 - v - u);
        w[1] = (F32)v;
        w[2] = (F32)u;
        dist = (t.p[0] * w[0] + t.p[1] * w[1] + t.p[2] * w[2] - p).length();
        return true;
    }

    // Every vertex of `from` lies on `onto` with the UV and normal `onto` interpolates there.
    std::string covers(const LLVolumeFace& from, const std::vector<Tri>& onto, bool check_attributes)
    {
        for (S32 i = 0; i < from.mNumVertices; ++i)
        {
            LLVector3 p = v3(from.mPositions[i]);
            LLVector3 n = v3(from.mNormals[i]);
            // An apex shared by collapsed triangles has neither a normal nor a single UV.
            bool has_normal = n.normVec() > 0.5f;
            bool on_surface = false;
            bool matched = false;
            for (const Tri& t : onto)
            {
                F32 w[3], dist;
                if (!project(t, p, dist, w) || dist > 1e-4f)
                {
                    continue;
                }
                on_surface = true;
                LLVector2 uv = t.uv[0] * w[0] + t.uv[1] * w[1] + t.uv[2] * w[2];
                LLVector3 tn = t.n[0] * w[0] + t.n[1] * w[1] + t.n[2] * w[2];
                tn.normVec();
                if (!check_attributes || !has_normal || ((uv - from.mTexCoords[i]).length() < 1e-3f && tn * n > 0.9999f))
                {
                    matched = true;
                    break;
                }
            }
            if (!on_surface || !matched)
            {
                std::ostringstream s;
                s << (on_surface ? "attributes differ" : "off surface") << " at vertex " << i << " " << p
                  << " uv " << from.mTexCoords[i] << " n " << n;
                return s.str();
            }
        }
        return {};
    }

    LLPointer<LLVolume> make(const LLVolumeParams& params, bool split)
    {
        LLVolume::sSplitFlatSides = split;
        LLPointer<LLVolume> volume = new LLVolume(params, 4.f, false, true);
        LLVolume::sSplitFlatSides = false;
        return volume;
    }

    S32 triangle_count(const LLVolume* volume)
    {
        S32 count = 0;
        for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
        {
            count += volume->getVolumeFace(f).mNumIndices / 3;
        }
        return count;
    }
}

namespace tut
{
    struct LLVolumeSplitData
    {
    };

    typedef test_group<LLVolumeSplitData> factory;
    typedef factory::object object;
}

namespace
{
    tut::factory llvolumesplit_test_factory("LLVolumeSplit");
}

namespace tut
{
    // Every combination of the line path's parameters on every flat-sided profile and hole.
    template<> template<>
    void object::test<1>()
    {
        const U8 profiles[] = { LL_PCODE_PROFILE_SQUARE, LL_PCODE_PROFILE_ISOTRI, LL_PCODE_PROFILE_EQUALTRI, LL_PCODE_PROFILE_RIGHTTRI };
        const U8 holes[] = { LL_PCODE_HOLE_SAME, LL_PCODE_HOLE_CIRCLE, LL_PCODE_HOLE_SQUARE, LL_PCODE_HOLE_TRIANGLE };
        const F32 hollows[] = { 0.f, 0.5f, 0.95f };
        const F32 profile_cuts[][2] = { { 0.f, 1.f }, { 0.125f, 0.8f }, { 0.3f, 0.45f } };
        const F32 path_cuts[][2] = { { 0.f, 1.f }, { 0.2f, 0.7f } };
        const F32 shears[][2] = { { 0.f, 0.f }, { 0.3f, -0.2f } };
        const F32 tapers[][2] = { { 0.f, 0.f }, { 0.25f, -0.4f } };
        const F32 ratios[][2] = { { 1.f, 1.f }, { 0.5f, 1.f }, { 0.f, 0.f } };

        S32 cases = 0, split_tris = 0, plain_tris = 0;
        for (U8 profile : profiles)
        for (U8 hole : holes)
        for (F32 hollow : hollows)
        for (auto& pcut : profile_cuts)
        for (auto& cut : path_cuts)
        for (auto& shear : shears)
        for (auto& taper : tapers)
        for (auto& ratio : ratios)
        {
            if (hollow == 0.f && hole != LL_PCODE_HOLE_SAME)
            {
                continue;
            }
            LLVolumeParams params;
            params.setType(profile | hole, LL_PCODE_PATH_LINE);
            params.setBeginAndEndS(pcut[0], pcut[1]);
            params.setBeginAndEndT(cut[0], cut[1]);
            params.setHollow(hollow);
            params.setShear(shear[0], shear[1]);
            params.setTaper(taper[0], taper[1]);
            params.setRatio(ratio[0], ratio[1]);

            LLPointer<LLVolume> split = make(params, true);
            LLPointer<LLVolume> plain = make(params, false);

            std::ostringstream what;
            what << "profile " << (S32)profile << " hole " << (S32)hole << " hollow " << hollow << " cut " << pcut[0] << "-"
                 << pcut[1] << " path " << cut[0] << "-" << cut[1] << " shear " << shear[0] << "," << shear[1] << " taper "
                 << taper[0] << "," << taper[1] << " ratio " << ratio[0] << "," << ratio[1];

            ensure_equals(what.str() + ": face count", plain->getNumVolumeFaces(), split->getNumVolumeFaces());
            for (S32 f = 0; f < split->getNumVolumeFaces(); ++f)
            {
                const LLVolumeFace& sf = split->getVolumeFace(f);
                const LLVolumeFace& pf = plain->getVolumeFace(f);
                std::ostringstream face;
                face << what.str() << " face " << f << " id " << sf.mID;
                ensure_equals(face.str() + ": face id", pf.mID, sf.mID);

                std::vector<Tri> st = triangles(sf), pt = triangles(pf);
                ensure_approximately_equals_range((face.str() + ": area").c_str(), area(pt), area(st), 1e-4f);
                if (area(pt) < 1e-6f)
                {
                    // A cap collapsed to a point by a zero top size has no surface to compare.
                    continue;
                }
                std::string err = covers(sf, pt, true);
                ensure(face.str() + ": split onto plain: " + err, err.empty());
                err = covers(pf, st, true);
                ensure(face.str() + ": plain onto split: " + err, err.empty());
                ensure(face.str() + ": extents", sf.mExtents[0].equals3(pf.mExtents[0], 1e-5f) &&
                                                      sf.mExtents[1].equals3(pf.mExtents[1], 1e-5f));
            }
            ++cases;
            split_tris += triangle_count(split);
            plain_tris += triangle_count(plain);
        }
        std::cout << "\nLLVolumeSplit: " << cases << " untwisted cases, " << split_tris << " triangles split, " << plain_tris
                  << " without" << std::endl;
        ensure("fewer triangles", plain_tris < split_tris);
    }

    // A twisted prim keeps its splits.
    template<> template<>
    void object::test<2>()
    {
        LLVolumeParams params;
        params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
        params.setBeginAndEndS(0.f, 1.f);
        params.setBeginAndEndT(0.f, 1.f);
        params.setRatio(1.f, 1.f);
        params.setTwistEnd(0.5f);
        ensure_equals("twisted box", triangle_count(make(params, false)), triangle_count(make(params, true)));
    }

    // A plain box is two triangles a side.
    template<> template<>
    void object::test<3>()
    {
        LLVolumeParams params;
        params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
        params.setBeginAndEndS(0.f, 1.f);
        params.setBeginAndEndT(0.f, 1.f);
        params.setRatio(1.f, 1.f);
        LLPointer<LLVolume> box = make(params, false);
        ensure_equals("box faces", box->getNumVolumeFaces(), 6);
        ensure_equals("box triangles", triangle_count(box), 12);
    }

    // What the common shapes cost at the highest detail, split and not.
    template<> template<>
    void object::test<4>()
    {
        struct Shape { const char* name; U8 profile; F32 hollow; F32 cut_end; };
        const Shape shapes[] = {
            { "box", LL_PCODE_PROFILE_SQUARE, 0.f, 1.f },
            { "hollow box", LL_PCODE_PROFILE_SQUARE, 0.5f, 1.f },
            { "cut box", LL_PCODE_PROFILE_SQUARE, 0.f, 0.75f },
            { "prism", LL_PCODE_PROFILE_EQUALTRI, 0.f, 1.f },
            { "hollow prism", LL_PCODE_PROFILE_EQUALTRI, 0.5f, 1.f },
        };
        for (const Shape& shape : shapes)
        {
            LLVolumeParams params;
            params.setType(shape.profile, LL_PCODE_PATH_LINE);
            params.setBeginAndEndS(0.f, shape.cut_end);
            params.setBeginAndEndT(0.f, 1.f);
            params.setRatio(1.f, 1.f);
            params.setHollow(shape.hollow);
            S32 split = triangle_count(make(params, true));
            S32 plain = triangle_count(make(params, false));
            std::cout << "\nLLVolumeSplit: " << shape.name << " " << split << " -> " << plain << " triangles";
            ensure(std::string(shape.name) + " fewer triangles", plain < split);
        }
        std::cout << std::endl;
    }

    // A volume regenerated in place (an edited prim's unique volume) matches a fresh one when its face sizes change.
    template<> template<>
    void object::test<5>()
    {
        const F32 hollows[] = { 0.f, 0.5f };
        const F32 cut_ends[] = { 1.f, 0.75f };
        for (bool from_split : { true, false })
        for (F32 hollow : hollows)
        for (F32 cut_end : cut_ends)
        {
            LLVolumeParams params;
            params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
            params.setBeginAndEndS(0.f, cut_end);
            params.setBeginAndEndT(0.f, 1.f);
            params.setRatio(1.f, 1.f);
            params.setHollow(hollow);

            LLPointer<LLVolume> volume = make(params, from_split);
            LLVolume::sSplitFlatSides = !from_split;
            volume->setDirty();
            volume->regen();
            LLVolume::sSplitFlatSides = false;
            LLPointer<LLVolume> fresh = make(params, !from_split);

            std::ostringstream what;
            what << (from_split ? "split to plain" : "plain to split") << " hollow " << hollow << " cut " << cut_end;
            ensure_equals(what.str() + ": face count", volume->getNumVolumeFaces(), fresh->getNumVolumeFaces());
            for (S32 f = 0; f < fresh->getNumVolumeFaces(); ++f)
            {
                const LLVolumeFace& rf = volume->getVolumeFace(f);
                const LLVolumeFace& ff = fresh->getVolumeFace(f);
                std::ostringstream face;
                face << what.str() << " face " << f;
                ensure_equals(face.str() + ": vertices", rf.mNumVertices, ff.mNumVertices);
                ensure_equals(face.str() + ": indices", rf.mNumIndices, ff.mNumIndices);
                for (S32 i = 0; i < ff.mNumIndices; ++i)
                {
                    ensure_equals(face.str() + ": index", rf.mIndices[i], ff.mIndices[i]);
                }
                std::string err = covers(rf, triangles(ff), true);
                ensure(face.str() + ": " + err, err.empty());
            }
        }
    }
}
