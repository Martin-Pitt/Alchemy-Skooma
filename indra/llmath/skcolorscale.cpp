/**
 * @file skcolorscale.cpp
 * @brief A mapping from data values to colours, shared by overlays, shaders and legends.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#include "skcolorscale.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    const LLColor4 NO_CLASS(0.5f, 0.5f, 0.5f, 1.f);

    LLColor4 mix(const LLColor4& a, const LLColor4& b, F32 t)
    {
        return LLColor4(a.mV[0] + (b.mV[0] - a.mV[0]) * t, a.mV[1] + (b.mV[1] - a.mV[1]) * t,
                        a.mV[2] + (b.mV[2] - a.mV[2]) * t, a.mV[3] + (b.mV[3] - a.mV[3]) * t);
    }
}

SKColorScale::SKColorScale(std::string id, Kind kind, std::vector<SKColorStop> stops)
    : mId(std::move(id)), mKind(kind), mStops(std::move(stops))
{
    if (isDiscrete())
    {
        mHi = mStops.empty() ? 0.0 : (F64)(mStops.size() - 1);
    }
}

SKColorScale SKColorScale::even(std::string id, Kind kind, const std::vector<LLColor4>& colours)
{
    std::vector<SKColorStop> stops;
    const size_t             n = colours.size();
    for (size_t i = 0; i < n; ++i)
    {
        stops.push_back({ n > 1 ? (F32)i / (F32)(n - 1) : 0.f, colours[i] });
    }
    return SKColorScale(std::move(id), kind, std::move(stops));
}

SKColorScale& SKColorScale::setDomain(F64 lo, F64 hi) { mLo = lo; mHi = hi; bump(); return *this; }
SKColorScale& SKColorScale::setMid(F64 mid) { mMid = mid; mHasMid = !std::isnan(mid); bump(); return *this; }
SKColorScale& SKColorScale::setTransform(Transform transform) { mTransform = transform; bump(); return *this; }
SKColorScale& SKColorScale::setUnit(std::string unit) { mUnit = std::move(unit); bump(); return *this; }
SKColorScale& SKColorScale::setStops(std::vector<SKColorStop> stops) { mStops = std::move(stops); bump(); return *this; }
SKColorScale& SKColorScale::setClasses(std::vector<std::string> names) { mClasses = std::move(names); bump(); return *this; }
SKColorScale& SKColorScale::setEdges(std::vector<F64> edges) { mEdges = std::move(edges); bump(); return *this; }
SKColorScale& SKColorScale::setUnder(const LLColor4& c) { mUnder = c; mHasUnder = true; bump(); return *this; }
SKColorScale& SKColorScale::setOver(const LLColor4& c) { mOver = c; mHasOver = true; bump(); return *this; }
SKColorScale& SKColorScale::clearClip() { mHasUnder = mHasOver = false; bump(); return *this; }

F64 SKColorScale::mid() const
{
    if (mHasMid)
    {
        return mMid;
    }
    return (mLo + mHi) * 0.5;
}

F64 SKColorScale::forward(F64 x) const
{
    switch (mTransform)
    {
        case Transform::Log:
            return x > 0.0 ? std::log10(x) : -std::numeric_limits<F64>::infinity();
        case Transform::Sqrt:
            return std::copysign(std::sqrt(std::fabs(x)), x);
        default:
            return x;
    }
}

F64 SKColorScale::unitOf(F64 value) const
{
    if (std::isnan(value))
    {
        return 0.0;
    }
    const F64 lo = forward(mLo), hi = forward(mHi), v = forward(value);
    if (mKind == Kind::Diverging)
    {
        const F64 m = mHasMid ? forward(mMid) : (lo + hi) * 0.5;
        return v < m ? 0.5 * (v - lo) / (m - lo) : 0.5 + 0.5 * (v - m) / (hi - m);
    }
    if (std::isinf(v) && v < 0.0)
    {
        return v;
    }
    return hi == lo ? 0.0 : (v - lo) / (hi - lo);
}

LLColor4 SKColorScale::at(F32 t) const
{
    if (mStops.empty())
    {
        return NO_CLASS;
    }
    if (!(t > mStops.front().t))
    {
        return mStops.front().c;
    }
    for (size_t i = 0; i + 1 < mStops.size(); ++i)
    {
        const SKColorStop& a = mStops[i];
        const SKColorStop& b = mStops[i + 1];
        if (t <= b.t)
        {
            return b.t == a.t ? a.c : mix(a.c, b.c, (t - a.t) / (b.t - a.t));
        }
    }
    return mStops.back().c;
}

S32 SKColorScale::classOf(F64 value) const
{
    if (std::isnan(value) || mStops.empty())
    {
        return -1;
    }
    const S32 n = (S32)mStops.size();
    if (mKind == Kind::Categorical)
    {
        const F64 i = std::round(value);
        return i >= 0.0 && i < (F64)n ? (S32)i : -1;
    }
    if (mKind == Kind::Binned)
    {
        if (mEdges.size() < 2 || value < mEdges.front() || value > mEdges.back())
        {
            return -1;
        }
        const S32 bins = std::min(n, (S32)mEdges.size() - 1);
        for (S32 i = 0; i < bins; ++i)
        {
            if (value < mEdges[i + 1])
            {
                return i;
            }
        }
        return bins - 1;
    }
    return -1;
}

LLColor4 SKColorScale::classColour(S32 index) const
{
    return index >= 0 && index < (S32)mStops.size() ? mStops[index].c : NO_CLASS;
}

std::string SKColorScale::className(S32 index) const
{
    return index >= 0 && index < (S32)mClasses.size() ? mClasses[index] : std::string();
}

LLColor4 SKColorScale::colour(F64 value) const
{
    if (isDiscrete())
    {
        const S32 i = classOf(value);
        if (i >= 0)
        {
            return mStops[i].c;
        }
        if (mKind == Kind::Categorical || std::isnan(value) || mStops.empty() || mEdges.empty())
        {
            return mHasUnder ? mUnder : NO_CLASS;
        }
        if (value < mEdges.front())
        {
            return mHasUnder ? mUnder : mStops.front().c;
        }
        return mHasOver ? mOver : mStops.back().c;
    }
    if (mKind == Kind::Identity)
    {
        const F32 g = llclamp((F32)value, 0.f, 1.f);
        return LLColor4(g, g, g, 1.f);
    }
    F64 t = unitOf(value);
    if (mKind == Kind::Cyclic)
    {
        t -= std::floor(t);
    }
    else if (t < 0.0)
    {
        if (mHasUnder)
        {
            return mUnder;
        }
        t = 0.0;
    }
    else if (t > 1.0)
    {
        if (mHasOver)
        {
            return mOver;
        }
        t = 1.0;
    }
    return at((F32)t);
}

U8 SKColorScale::toByte(F32 c)
{
    return (U8)std::floor(llclamp(c, 0.f, 1.f) * 255.f + 0.5f);
}

std::vector<U8> SKColorScale::bake(S32 n) const
{
    std::vector<U8> out;
    if (n <= 0)
    {
        return out;
    }
    out.reserve((size_t)n * 4);
    const S32 classes = (S32)mStops.size();
    for (S32 i = 0; i < n; ++i)
    {
        const F32 t = ((F32)i + 0.5f) / (F32)n;
        LLColor4  c;
        if (isDiscrete())
        {
            c = classColour(std::min(classes - 1, (S32)(t * (F32)classes)));
        }
        else if (mKind == Kind::Identity)
        {
            c = LLColor4(t, t, t, 1.f);
        }
        else
        {
            c = at(t);
        }
        for (S32 k = 0; k < 4; ++k)
        {
            out.push_back(toByte(c.mV[k]));
        }
    }
    return out;
}

SKColorScale::Uniforms SKColorScale::uniforms() const
{
    Uniforms u;
    u.kind      = (S32)mKind;
    u.transform = (S32)mTransform;
    u.count     = std::min((S32)mStops.size(), MAX_UNIFORM_STOPS);
    u.lo        = (F32)forward(mLo);
    u.hi        = (F32)forward(mHi);
    u.mid       = (F32)(mHasMid ? forward(mMid) : (forward(mLo) + forward(mHi)) * 0.5);
    u.under     = mHasUnder ? mUnder : LLColor4(0.f, 0.f, 0.f, 0.f);
    u.over      = mHasOver ? mOver : LLColor4(0.f, 0.f, 0.f, 0.f);
    for (S32 i = 0; i < u.count; ++i)
    {
        u.t[i] = mStops[i].t;
        u.c[i] = mStops[i].c;
    }
    for (S32 i = 0; i < std::min((S32)mEdges.size(), MAX_UNIFORM_STOPS + 1); ++i)
    {
        u.edges[i] = (F32)mEdges[i];
    }
    return u;
}

namespace SKColorScales
{
    namespace
    {
        LLColor4 rgb(F32 r, F32 g, F32 b) { return LLColor4(r, g, b, 1.f); }

        SKColorScale makeSeries()
        {
            SKColorScale s = SKColorScale::even("series", SKColorScale::Kind::Categorical,
                { rgb(.20f, .90f, 1.f), rgb(1.f, .30f, .85f), rgb(1.f, .70f, .20f), rgb(.70f, .45f, 1.f),
                  rgb(.95f, .95f, .30f), rgb(1.f, .30f, .30f), rgb(.30f, 1.f, .45f), rgb(1.f, 1.f, 1.f) });
            s.setClasses({ "cyan", "magenta", "amber", "violet", "yellow", "red", "green", "white" });
            return s;
        }
    }

    const SKColorScale& series()
    {
        static const SKColorScale s = makeSeries();
        return s;
    }

    const SKColorScale& ink()
    {
        static const SKColorScale s = SKColorScale::even("ink", SKColorScale::Kind::Sequential, { rgb(.30f, .30f, .32f), rgb(1.f, 1.f, 1.f) });
        return s;
    }

    const SKColorScale& heat()
    {
        static const SKColorScale s = SKColorScale::even("heat", SKColorScale::Kind::Sequential,
            { rgb(1.f, .15f, .10f), rgb(1.f, .90f, .20f), rgb(.20f, .90f, 1.f) });
        return s;
    }

    const SKColorScale& teal()
    {
        static const SKColorScale s = SKColorScale::even("teal", SKColorScale::Kind::Sequential, { rgb(.02f, .20f, .24f), rgb(.55f, .96f, .92f) });
        return s;
    }

    const SKColorScale& ember()
    {
        static const SKColorScale s = SKColorScale::even("ember", SKColorScale::Kind::Sequential, { rgb(1.f, .62f, .10f), rgb(1.f, .08f, .05f) });
        return s;
    }

    const SKColorScale& water()
    {
        static const SKColorScale s = SKColorScale::even("water", SKColorScale::Kind::Sequential,
            { rgb(.10f, .15f, .35f), rgb(.20f, .45f, .80f), rgb(.45f, .85f, 1.f) });
        return s;
    }

    const SKColorScale& signedScale()
    {
        static const SKColorScale s = SKColorScale::even("signed", SKColorScale::Kind::Diverging,
            { rgb(.20f, .90f, 1.f), rgb(.30f, .30f, .32f), rgb(1.f, .70f, .20f) }).setDomain(-1.0, 1.0);
        return s;
    }

    const SKColorScale& hue()
    {
        static const SKColorScale s = SKColorScale::even("hue", SKColorScale::Kind::Cyclic,
            { rgb(1.f, .30f, .30f), rgb(.95f, .95f, .30f), rgb(.30f, 1.f, .45f), rgb(.20f, .90f, 1.f),
              rgb(.70f, .45f, 1.f), rgb(1.f, .30f, .85f), rgb(1.f, .30f, .30f) }).setDomain(0.0, 360.0).setUnit("deg");
        return s;
    }

    const std::vector<std::string>& names()
    {
        static const std::vector<std::string> n = { "series", "ink", "heat", "teal", "ember", "water", "signed", "hue" };
        return n;
    }

    const SKColorScale* byName(const std::string& id)
    {
        for (const SKColorScale* s : { &series(), &ink(), &heat(), &teal(), &ember(), &water(), &signedScale(), &hue() })
        {
            if (s->id() == id)
            {
                return s;
            }
        }
        return nullptr;
    }
}
