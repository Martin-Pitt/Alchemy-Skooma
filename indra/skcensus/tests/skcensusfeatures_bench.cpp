/**
 * @file skcensusfeatures_bench.cpp
 * @brief What the census's feature pass costs a scan, by stage, on a synthetic region.
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

// A synthetic region: houses on a 16 m grid, each four box walls, a mesh
// wall of a few hundred triangles, a floor, two scripted doors and a named
// painting, with furniture strewn between them. The pass is timed as the
// census ran it, every candidate cast against a walk of every record, then
// over the record index and over the cache a second scan finds warm.

#include "linden_common.h"

#include "../skcensusfeatures.h"

#include "altestbenchmark.h"

#include <random>
#include <vector>

namespace
{
    using namespace skworld;

    LLUUID id(U32 n)
    {
        LLUUID u;
        u.mData[0] = static_cast<U8>(n);
        u.mData[1] = static_cast<U8>(n >> 8);
        u.mData[2] = static_cast<U8>(n >> 16);
        return u;
    }

    struct Scene
    {
        std::vector<RecordPtr> mRecords;
        std::vector<FeaturePart> mParts;
        std::vector<FeatureRoot> mRoots;
    };

    // One axis-aligned box as both a record and a part.
    void box(Scene& s, U32 root, U32 n, LLVector3d lo, LLVector3d hi, bool mesh, const char* name, bool scripted)
    {
        auto r = std::make_shared<Record>();
        r->mPart = id(n);
        r->mRoot = id(root);
        r->mMin = lo;
        r->mMax = hi;
        const LLVector3 l(lo), h(hi);
        r->mCenter = (l + h) * 0.5f;
        r->mHalf = (h - l) * 0.5f;
        r->mAxes[0].set(1.f, 0.f, 0.f);
        r->mAxes[1].set(0.f, 1.f, 0.f);
        r->mAxes[2].set(0.f, 0.f, 1.f);
        r->mShape = Shape::BOX;
        if (mesh)
        {
            // A panelled wall: a grid of quads over both big faces.
            auto tris = std::make_shared<std::vector<LLVector3>>();
            const S32 N = 12;
            for (const F32 x : { l.mV[VX], h.mV[VX] })
            {
                for (S32 i = 0; i < N; ++i)
                {
                    for (S32 j = 0; j < N; ++j)
                    {
                        const F32 y0 = l.mV[VY] + (h.mV[VY] - l.mV[VY]) * i / N, y1 = l.mV[VY] + (h.mV[VY] - l.mV[VY]) * (i + 1) / N;
                        const F32 z0 = l.mV[VZ] + (h.mV[VZ] - l.mV[VZ]) * j / N, z1 = l.mV[VZ] + (h.mV[VZ] - l.mV[VZ]) * (j + 1) / N;
                        tris->insert(tris->end(), { LLVector3(x, y0, z0), LLVector3(x, y1, z0), LLVector3(x, y1, z1), LLVector3(x, y0, z0),
                                                    LLVector3(x, y1, z1), LLVector3(x, y0, z1) });
                    }
                }
            }
            r->mTriangles = tris;
            r->mShape = Shape::TRIANGLES;
        }
        s.mRecords.push_back(r);

        FeaturePart p;
        p.mPart = r->mPart;
        p.mRoot = r->mRoot;
        p.mMin = lo;
        p.mMax = hi;
        p.mCenter = (lo + hi) * 0.5;
        p.mAxes[0].set(1.f, 0.f, 0.f);
        p.mAxes[1].set(0.f, 1.f, 0.f);
        p.mAxes[2].set(0.f, 0.f, 1.f);
        p.mHalf = r->mHalf;
        p.mName = name;
        p.mScripted = scripted;
        p.mTouch = scripted;
        s.mParts.push_back(p);
    }

    Scene region(U32 furniture)
    {
        Scene s;
        std::mt19937 rng(7);
        std::uniform_real_distribution<F64> u(0.0, 1.0);
        U32 n = 1, root = 1;
        for (S32 gx = 0; gx < 16; ++gx)
        {
            for (S32 gy = 0; gy < 16; ++gy, ++root)
            {
                const F64 x = gx * 16.0 + 2.0, y = gy * 16.0 + 2.0, z = 22.0;
                FeatureRoot fr;
                fr.mRoot = id(root);
                fr.mName = "House";
                s.mRoots.push_back(fr);
                box(s, root, n++, { x, y, z - 0.2 }, { x + 12, y + 12, z }, false, "", false);
                box(s, root, n++, { x, y, z }, { x + 0.2, y + 12, z + 3.5 }, true, "", false);
                box(s, root, n++, { x + 11.8, y, z }, { x + 12, y + 12, z + 3.5 }, false, "", false);
                box(s, root, n++, { x, y, z }, { x + 5, y + 0.2, z + 3.5 }, false, "", false);
                box(s, root, n++, { x + 6.2, y, z }, { x + 12, y + 0.2, z + 3.5 }, false, "", false);
                box(s, root, n++, { x, y + 11.8, z }, { x + 12, y + 12, z + 3.5 }, false, "", false);
                // Doors in the gap of the front wall and inside, and a painting on the mesh wall.
                box(s, root, n++, { x + 5.05, y + 0.05, z }, { x + 6.15, y + 0.15, z + 2.4 }, false, "", true);
                box(s, root, n++, { x + 6, y + 6, z }, { x + 6.1, y + 7.1, z + 2.3 }, false, "Door", true);
                box(s, root, n++, { x + 0.2, y + 3, z + 0.5 }, { x + 0.25, y + 4.2, z + 3 }, false, "Painting", false);
            }
        }
        // Ladders of three pieces, and elevators: three landing doors and a car.
        for (S32 k = 0; k < 24; ++k, ++root)
        {
            const F64 x = 8.0 + k * 10.0, y = 14.5, z = 22.0;
            FeatureRoot fr;
            fr.mRoot = id(root);
            fr.mName = "Rusty Ladder";
            s.mRoots.push_back(fr);
            for (S32 j = 0; j < 3; ++j)
            {
                box(s, root, n++, { x, y, z + j * 2.0 }, { x + 0.3, y + 0.8, z + j * 2.0 + 2.1 }, false, "", false);
            }
        }
        for (S32 k = 0; k < 8; ++k)
        {
            const F64 x = 14.5 + k * 30.0, y = 30.5, z = 22.0;
            for (S32 j = 0; j < 3; ++j, ++root)
            {
                FeatureRoot fr;
                fr.mRoot = id(root);
                s.mRoots.push_back(fr);
                box(s, root, n++, { x, y, z + j * 4.0 }, { x + 0.1, y + 1.1, z + j * 4.0 + 2.5 }, false, "Elevator door", true);
            }
            FeatureRoot car;
            car.mRoot = id(root);
            car.mName = "Elevator";
            car.mScripted = TriState::YES;
            s.mRoots.push_back(car);
            box(s, root++, n++, { x + 0.3, y - 0.5, z + 4.0 }, { x + 2.3, y + 1.5, z + 6.5 }, false, "", true);
        }
        for (U32 i = 0; i < furniture; ++i, ++root)
        {
            const F64 x = u(rng) * 256.0, y = u(rng) * 256.0, z = 22.0;
            const F64 w = 0.2 + u(rng) * 1.5, d = 0.2 + u(rng) * 1.5, h = 0.3 + u(rng) * 2.5;
            FeatureRoot fr;
            fr.mRoot = id(root);
            // Some of it sat on this session, a few driven off.
            fr.mEverCrewed = i % 200 == 0;
            fr.mTravel = i % 1000 == 0 ? 20.f : 0.f;
            s.mRoots.push_back(fr);
            box(s, root, n++, { x, y, z }, { x + w, y + d, z + h }, i % 7 == 0, i % 3 == 0 ? "Chair" : "", i % 5 == 0);
        }
        return s;
    }

    bool candidate(const FeaturePart& p, const FeatureParams& params)
    {
        return (p.mScripted || p.mTouch || !p.mName.empty()) && doorShaped(p, params);
    }

    TEST_CASE("skcensusfeatures_bench: face cover and classification over a region of houses", "[skcensusfeatures_bench][!benchmark]")
    {
        ll_test::skipUnlessOptimised();
        const FeatureParams params;
        Scene s = region(12000);
        std::vector<size_t> candidates;
        for (size_t i = 0; i < s.mParts.size(); ++i)
        {
            if (candidate(s.mParts[i], params))
            {
                candidates.push_back(i);
            }
        }
        INFO(s.mRecords.size() << " records, " << candidates.size() << " candidates");
        const RecordIndex index(s.mRecords);
        FaceCoverCache cache;
        for (size_t i : candidates)
        {
            const F32 walked = faceCover(s.mParts[i], s.mRecords, params);
            CHECK(faceCover(s.mParts[i], index, params) == walked);
            CHECK(cache.measure(s.mParts[i], index, params) == walked);
            s.mParts[i].mFaceCover = walked;
        }
        cache.prune();
        BENCHMARK("face cover, every record walked")
        {
            F32 sum = 0.f;
            for (size_t i : candidates)
            {
                sum += faceCover(s.mParts[i], s.mRecords, params);
            }
            return sum;
        };
        BENCHMARK("record index built")
        {
            const RecordIndex built(s.mRecords);
            std::vector<const RecordPtr*> found;
            built.query(s.mParts[0].mMin, s.mParts[0].mMax, found);
            return found.size();
        };
        BENCHMARK("face cover over the index")
        {
            F32 sum = 0.f;
            for (size_t i : candidates)
            {
                sum += faceCover(s.mParts[i], index, params);
            }
            return sum;
        };
        BENCHMARK("face cover cached, warm scan")
        {
            F32 sum = 0.f;
            for (size_t i : candidates)
            {
                sum += cache.measure(s.mParts[i], index, params);
            }
            cache.prune();
            return sum;
        };
        const std::vector<Feature> features = classifyFeatures(s.mParts, s.mRoots, params);
        BENCHMARK("door entities")
        {
            return buildDoorEntities(features, s.mParts, s.mRoots).size();
        };
        BENCHMARK("classify features")
        {
            return classifyFeatures(s.mParts, s.mRoots, params).size();
        };
    }
}
