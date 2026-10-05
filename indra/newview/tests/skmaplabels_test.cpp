/**
 * @file skmaplabels_test.cpp
 * @brief Mini-map hover hysteresis and nametag placement against the study's own loop.
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

#include "../skmaplabels.h"

#include <algorithm>
#include <cmath>

namespace
{
    LLUUID idOf(S32 i)
    {
        LLUUID id;
        id.mData[0] = static_cast<U8>(i + 1);
        id.mData[15] = static_cast<U8>(0x40 + i);
        return id;
    }

    /// A deterministic generator for the random scenes.
    struct Lcg
    {
        U64 mState;
        explicit Lcg(U64 seed) : mState(seed) {}
        F64 next()
        {
            mState = mState * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<F64>((mState >> 11) & ((1ull << 53) - 1)) / static_cast<F64>(1ull << 53);
        }
    };

    /// The study's crowded-edge scene: twelve long names in a corner map 240 px wide, y down.
    struct EdgeScene
    {
        std::vector<LLVector2> mDots;
        skmap::LabelRect mCog, mGrip;
        F64 mW, mH;
        skmap::LabelProblem mProblem;

        EdgeScene(F64 w, F64 h, F64 max_width)
            : mW(w), mH(h)
        {
            const F64 size = std::min(240.0, w - 32.0);
            const F64 mx = w - size - 16.0, my = 16.0, k = size / 96.0;
            static const char* NAMES[12] = { "Alexandra Nightingale", "Morgan Rivers", "Morgan Rivers", "Juniper Northwind",
                                             "Sasha Vale", "Emery Winterbourne", "Rowan Ellis", "Cassidy Moon",
                                             "Dakota Lindenfield", "Kai Evermore", "Avery Stone", "Quinn Silver" };
            mCog = { mx + size - 28.0, 18.0, 26.0, 26.0 };
            mGrip = { mx + size - 26.0, my + size - 26.0, 26.0, 26.0 };
            for (S32 i = 0; i < 12; ++i)
            {
                const F64 x = 24.0 + (i % 4) * 2.0, y = 29.0 + (i / 4) * 2.0;
                mDots.emplace_back(static_cast<F32>(mx + size / 2.0 + x * k), static_cast<F32>(my + size / 2.0 - y * k));
            }
            LLVector2 centre;
            for (const LLVector2& d : mDots)
            {
                centre += d / 12.f;
            }
            mProblem.mCentre = centre;
            mProblem.mWidth = w;
            mProblem.mHeight = h;
            for (const LLVector2& d : mDots)
            {
                mProblem.mObstacles.push_back({ d.mV[VX] - 5.0, d.mV[VY] - 5.0, 10.0, 10.0 });
            }
            mProblem.mObstacles.push_back(mCog);
            mProblem.mObstacles.push_back(mGrip);
            for (S32 i = 0; i < 12; ++i)
            {
                skmap::LabelInput li;
                li.mAnchor = mDots[i];
                // About 6.5 px a character at 12 px, padded 6 a side; usernames add a line.
                li.mW = std::min(max_width, std::strlen(NAMES[i]) * 6.5 + 12.0);
                li.mH = (i == 1 || i == 2) ? 35.0 : 21.0;
                mProblem.mLabels.push_back(li);
            }
        }
    };

    void ensureClean(const char* what, const std::vector<skmap::LabelRect>& rects, const EdgeScene& s)
    {
        for (size_t i = 0; i < rects.size(); ++i)
        {
            const skmap::LabelRect& a = rects[i];
            tut::ensure(std::string(what) + ": in bounds",
                        a.mX >= 8.0 - 1e-9 && a.mY >= 8.0 - 1e-9 && a.mX + a.mW <= s.mW - 8.0 + 1e-9 && a.mY + a.mH <= s.mH - 8.0 + 1e-9);
            tut::ensure(std::string(what) + ": clear of the cog", !skmap::rectOverlap(a, s.mCog, 0.0));
            tut::ensure(std::string(what) + ": clear of the grip", !skmap::rectOverlap(a, s.mGrip, 0.0));
            for (size_t j = i + 1; j < rects.size(); ++j)
            {
                tut::ensure(std::string(what) + ": no overlap", !skmap::rectOverlap(a, rects[j], 0.0));
            }
        }
    }
}

namespace tut
{
    struct skmaplabels_data {};
    typedef test_group<skmaplabels_data> skmaplabels_group;
    typedef skmaplabels_group::object skmaplabels_object;
    tut::skmaplabels_group g("skmaplabels");

    // Enter at 10, not at 10.5; a member stays at 11.9 and leaves at 12.1; suppression lifts on a move.
    template<> template<>
    void skmaplabels_object::test<1>()
    {
        skmap::HoverSet set;
        const LLVector2 pointer(100.f, 100.f);
        set.update({ { idOf(0), LLVector2(110.5f, 100.f) } }, pointer);
        ensure("not at 10.5", set.empty());
        set.update({ { idOf(0), LLVector2(110.f, 100.f) } }, pointer);
        ensure_equals("enters at 10", set.members().size(), static_cast<size_t>(1));
        set.update({ { idOf(0), LLVector2(111.9f, 100.f) } }, pointer);
        ensure_equals("stays at 11.9", set.members().size(), static_cast<size_t>(1));
        set.update({ { idOf(0), LLVector2(112.1f, 100.f) } }, pointer);
        ensure("leaves at 12.1", set.empty());

        set.update({ { idOf(0), LLVector2(105.f, 100.f) } }, pointer);
        set.suppress(pointer);
        ensure("suppressed clears", set.empty() && set.suppressed());
        ensure("same position holds it", !set.notePointer(pointer));
        ensure("a move lifts it", set.notePointer(pointer + LLVector2(1.f, 0.f)) && !set.suppressed());
    }

    // Members by distance, then id, whatever order the dots came in.
    template<> template<>
    void skmaplabels_object::test<2>()
    {
        std::vector<skmap::HoverDot> dots = { { idOf(3), LLVector2(4.f, 0.f) }, { idOf(1), LLVector2(2.f, 0.f) },
                                              { idOf(2), LLVector2(0.f, 2.f) }, { idOf(0), LLVector2(0.f, 8.f) } };
        skmap::HoverSet a, b;
        a.update(dots, LLVector2(0.f, 0.f));
        std::reverse(dots.begin(), dots.end());
        b.update(dots, LLVector2(0.f, 0.f));
        ensure("order-independent", a.members() == b.members());
        ensure_equals("four", a.members().size(), static_cast<size_t>(4));
        ensure("ties by id", a.members()[0] == std::min(idOf(1), idOf(2)));
        ensure("then distance", a.members()[2] == idOf(3) && a.members()[3] == idOf(0));
    }

    // Twelve long names on the narrow stage, then a wide window: clean either way.
    template<> template<>
    void skmaplabels_object::test<3>()
    {
        ensure_equals("narrow width rule", skmap::tagMaxWidth(320.f, 190.f), 145.f);
        ensure_equals("wide width rule", skmap::tagMaxWidth(1280.f, 190.f), 190.f);
        const EdgeScene narrow(320.0, 560.0, skmap::tagMaxWidth(320.f, 190.f));
        ensureClean("320 x 560", skmap::solve(narrow.mProblem), narrow);
        const EdgeScene wide(1280.0, 720.0, skmap::tagMaxWidth(1280.f, 190.f));
        ensureClean("1280 x 720", skmap::solve(wide.mProblem), wide);
    }

    // The previous target is kept while it is still free.
    template<> template<>
    void skmaplabels_object::test<4>()
    {
        EdgeScene s(1280.0, 720.0, 190.0);
        const std::vector<skmap::LabelRect> first = skmap::solve(s.mProblem);
        for (size_t i = 0; i < first.size(); ++i)
        {
            s.mProblem.mLabels[i].mFresh = false;
            s.mProblem.mLabels[i].mPrevX = first[i].mX;
            s.mProblem.mLabels[i].mPrevY = first[i].mY;
        }
        // A dot far off joins the obstacles: every tag keeps its place.
        s.mProblem.mObstacles.push_back({ 20.0, 690.0, 10.0, 10.0 });
        const std::vector<skmap::LabelRect> second = skmap::solve(s.mProblem);
        ensure("kept", first == second);
        // Once something takes the first tag's place, it moves.
        s.mProblem.mObstacles.push_back(first.front());
        const std::vector<skmap::LabelRect> third = skmap::solve(s.mProblem);
        ensure("moved off a taken place", !(third.front() == first.front()));
    }

    // Coincident dots fan out round the pointer.
    template<> template<>
    void skmaplabels_object::test<5>()
    {
        skmap::LabelProblem p;
        p.mCentre.set(400.f, 300.f);
        p.mWidth = 800.0;
        p.mHeight = 600.0;
        for (S32 i = 0; i < 4; ++i)
        {
            skmap::LabelInput li;
            li.mAnchor = p.mCentre;
            li.mW = 60.0;
            li.mH = 20.0;
            p.mLabels.push_back(li);
        }
        p.mObstacles.push_back({ 395.0, 295.0, 10.0, 10.0 });
        const std::vector<skmap::LabelRect> rects = skmap::solve(p);
        std::set<S32> quadrants;
        for (const skmap::LabelRect& r : rects)
        {
            const F64 dx = r.mX + r.mW / 2.0 - 400.0, dy = r.mY + r.mH / 2.0 - 300.0;
            quadrants.insert(static_cast<S32>(std::floor(std::atan2(dy, dx) / (F_PI / 2.0))));
        }
        ensure("spread round the pointer", quadrants.size() >= 3);
        for (size_t i = 0; i < rects.size(); ++i)
        {
            for (size_t j = i + 1; j < rects.size(); ++j)
            {
                ensure("apart", !skmap::rectOverlap(rects[i], rects[j], 0.0));
            }
        }
    }

    // A dot up and right of the pointer gets its tag up and right, in GL's y-up frame.
    template<> template<>
    void skmaplabels_object::test<6>()
    {
        const F32 height = 600.f;
        const LLVector2 pointer_gl(300.f, 300.f), dot_gl(308.f, 306.f);
        auto down = [&](const LLVector2& v) { return LLVector2(v.mV[VX], height - v.mV[VY]); };
        skmap::LabelProblem p;
        p.mCentre = down(pointer_gl);
        p.mWidth = 800.0;
        p.mHeight = height;
        skmap::LabelInput li;
        li.mAnchor = down(dot_gl);
        li.mW = 80.0;
        li.mH = 20.0;
        p.mLabels.push_back(li);
        p.mObstacles.push_back({ li.mAnchor.mV[VX] - 5.0, li.mAnchor.mV[VY] - 5.0, 10.0, 10.0 });
        const skmap::LabelRect r = skmap::solve(p).front();
        const F64 cx = r.mX + r.mW / 2.0, cy_gl = height - (r.mY + r.mH / 2.0);
        ensure("right", cx > pointer_gl.mV[VX]);
        ensure("up", cy_gl > pointer_gl.mV[VY]);
    }

    // The pruned search equals the study's loop on 200 random scenes.
    template<> template<>
    void skmaplabels_object::test<7>()
    {
        Lcg rng(1234567);
        for (S32 scene = 0; scene < 200; ++scene)
        {
            skmap::LabelProblem p;
            p.mWidth = 300.0 + rng.next() * 1300.0;
            p.mHeight = 300.0 + rng.next() * 700.0;
            p.mCentre.set(static_cast<F32>(20.0 + rng.next() * (p.mWidth - 40.0)), static_cast<F32>(20.0 + rng.next() * (p.mHeight - 40.0)));
            const S32 count = 1 + static_cast<S32>(rng.next() * 9.0);
            for (S32 i = 0; i < count; ++i)
            {
                skmap::LabelInput li;
                li.mAnchor = p.mCentre + LLVector2(static_cast<F32>(rng.next() * 24.0 - 12.0), static_cast<F32>(rng.next() * 24.0 - 12.0));
                li.mW = 40.0 + rng.next() * 150.0;
                li.mH = rng.next() < 0.3 ? 35.0 : 21.0;
                li.mFresh = rng.next() < 0.5;
                li.mPrevX = rng.next() * p.mWidth;
                li.mPrevY = rng.next() * p.mHeight;
                p.mLabels.push_back(li);
                p.mObstacles.push_back({ li.mAnchor.mV[VX] - 5.0, li.mAnchor.mV[VY] - 5.0, 10.0, 10.0 });
            }
            p.mObstacles.push_back({ p.mWidth - 40.0, 18.0, 26.0, 26.0 });
            ensure("pruned == brute force", skmap::solve(p) == skmap::solveBruteForce(p));
        }
    }

    // Tags snap in when fresh, ease towards a new target, and solve only when something moved.
    template<> template<>
    void skmaplabels_object::test<8>()
    {
        skmap::LabelLayout layout;
        skmap::LabelLayout::Input in;
        in.mWidth = 800.f;
        in.mHeight = 600.f;
        in.mPointer.set(400.f, 300.f);
        in.mMembers.push_back({ idOf(0), LLVector2(410.f, 300.f), 80.f, 20.f });
        layout.update(in, 0.016f);
        ensure_equals("one solve", layout.solves(), 1u);
        const skmap::LabelLayout::Tag first = layout.tags().front();
        ensure("fresh snaps", first.mX == first.mTX && first.mY == first.mTY);
        layout.update(in, 0.016f);
        ensure_equals("nothing moved, no solve", layout.solves(), 1u);
        in.mPointer.set(400.5f, 300.f);
        layout.update(in, 0.016f);
        ensure_equals("half a pixel, no solve", layout.solves(), 1u);
        in.mMembers.front().mAnchor.set(300.f, 200.f);
        layout.update(in, 0.016f);
        ensure_equals("the dot moved", layout.solves(), 2u);
        const skmap::LabelLayout::Tag eased = layout.tags().front();
        ensure("eases rather than jumps", eased.mX != eased.mTX || eased.mY != eased.mTY);
        const LLVector2 end = skmap::LabelLayout::connectorEnd(eased, LLVector2(300.f, 200.f));
        ensure("connector meets the box", end.mV[VX] >= eased.mX - 1e-3 && end.mV[VX] <= eased.mX + eased.mW + 1e-3);
    }
}
