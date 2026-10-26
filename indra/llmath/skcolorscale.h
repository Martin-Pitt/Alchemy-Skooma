/**
 * @file skcolorscale.h
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

#ifndef SK_COLORSCALE_H
#define SK_COLORSCALE_H

#include "v4color.h"

#include <array>
#include <string>
#include <vector>

// One colour at a position t in [0, 1] along a scale.
struct SKColorStop
{
    F32      t = 0.f;
    LLColor4 c;
};

// What a drawer colours by and what its legend explains, as one object.
// Interpolation is a plain sRGB lerp between stops, as the viewer's ramps have always done.
class SKColorScale
{
public:
    enum class Kind : U8
    {
        Sequential,  // low to high
        Diverging,   // two arms around a neutral mid
        Cyclic,      // wraps, as a hue or an angle
        Categorical, // one stop a class, the value is the class index
        Binned,      // one stop a class, classes by value edges
        Identity     // data drawn as its own colour; legends show its channels
    };

    enum class Transform : U8
    {
        Linear,
        Log,
        Sqrt
    };

    // The most stops a scale passes to a shader.
    static constexpr S32 MAX_UNIFORM_STOPS = 16;

    // What a shader needs to evaluate the scale as C++ does.
    struct Uniforms
    {
        S32                                     kind      = 0;
        S32                                     transform = 0;
        S32                                     count     = 0;
        F32                                     lo = 0.f, mid = 0.f, hi = 1.f; // transformed
        LLColor4                                under, over;                  // alpha 0: clamp
        std::array<F32, MAX_UNIFORM_STOPS>      t {};
        std::array<LLColor4, MAX_UNIFORM_STOPS> c {};
        std::array<F32, MAX_UNIFORM_STOPS + 1>  edges {};                     // Binned only
    };

    SKColorScale() = default;
    SKColorScale(std::string id, Kind kind, std::vector<SKColorStop> stops);
    // Stops evenly spaced from t 0 to 1.
    static SKColorScale even(std::string id, Kind kind, const std::vector<LLColor4>& colours);

    // Setters bump the revision, so a consumer holding the scale knows to rebuild.
    SKColorScale& setDomain(F64 lo, F64 hi);
    SKColorScale& setMid(F64 mid);
    SKColorScale& setTransform(Transform transform);
    SKColorScale& setUnit(std::string unit);
    SKColorScale& setStops(std::vector<SKColorStop> stops);
    SKColorScale& setClasses(std::vector<std::string> names);
    SKColorScale& setEdges(std::vector<F64> edges);
    SKColorScale& setUnder(const LLColor4& c);
    SKColorScale& setOver(const LLColor4& c);
    SKColorScale& clearClip();

    const std::string&              id() const { return mId; }
    Kind                            kind() const { return mKind; }
    Transform                       transform() const { return mTransform; }
    const std::string&              unit() const { return mUnit; }
    F64                             lo() const { return mLo; }
    F64                             hi() const { return mHi; }
    F64                             mid() const;
    const std::vector<SKColorStop>& stops() const { return mStops; }
    const std::vector<std::string>& classes() const { return mClasses; }
    const std::vector<F64>&         edges() const { return mEdges; }
    bool                            hasUnder() const { return mHasUnder; }
    bool                            hasOver() const { return mHasOver; }
    const LLColor4&                 under() const { return mUnder; }
    const LLColor4&                 over() const { return mOver; }
    bool                            isDiscrete() const { return mKind == Kind::Categorical || mKind == Kind::Binned; }
    S32                             classCount() const { return isDiscrete() ? (S32)mStops.size() : 0; }
    U32                             revision() const { return mRevision; }

    // Value to position on the scale, unclamped; NaN gives 0, a log of nothing positive -inf.
    F64      unitOf(F64 value) const;
    // Position to colour along the stops, clamped to the ends.
    LLColor4 at(F32 t) const;
    // Value to colour, with the clip colours where set; a class colour for discrete kinds.
    LLColor4 colour(F64 value) const;
    // The class of a value for discrete kinds, -1 for none.
    S32      classOf(F64 value) const;
    // The colour of a class, grey where there is none.
    LLColor4 classColour(S32 index) const;
    // The name of a class, empty where there is none.
    std::string className(S32 index) const;

    // An RGBA8 1-D LUT of n texels sampled at their centres in t; discrete kinds pick class floor(t * count).
    std::vector<U8> bake(S32 n) const;
    // The scale as shader constants, stops beyond MAX_UNIFORM_STOPS dropped.
    Uniforms uniforms() const;

    // A scale that samples a function at n + 1 even positions.
    template <typename F> static SKColorScale sampled(std::string id, F fn, S32 n = 32)
    {
        std::vector<SKColorStop> stops;
        for (S32 i = 0; i <= n; ++i)
        {
            const F32 t = (F32)i / (F32)n;
            stops.push_back({ t, fn(t) });
        }
        return SKColorScale(std::move(id), Kind::Sequential, std::move(stops));
    }

    static U8 toByte(F32 c);

private:
    F64 forward(F64 x) const;
    void bump() { ++mRevision; }

    std::string              mId;
    std::string              mUnit;
    Kind                     mKind      = Kind::Sequential;
    Transform                mTransform = Transform::Linear;
    F64                      mLo = 0.0, mHi = 1.0, mMid = 0.0;
    bool                     mHasMid   = false;
    bool                     mHasUnder = false;
    bool                     mHasOver  = false;
    LLColor4                 mUnder, mOver;
    std::vector<SKColorStop> mStops;
    std::vector<std::string> mClasses;
    std::vector<F64>         mEdges;
    U32                      mRevision = 0;
};

// The built-in scales, by name, so an overlay, its shader and its legend ask for the same one.
namespace SKColorScales
{
    const SKColorScale& series(); // Categorical: cyan magenta amber violet yellow red green white
    const SKColorScale& ink();    // Sequential presence / coverage / density
    const SKColorScale& heat();   // Sequential red, yellow, cyan
    const SKColorScale& teal();   // Sequential speed
    const SKColorScale& ember();  // Sequential energy / time cost
    const SKColorScale& water();  // Sequential moisture / depth
    const SKColorScale& signedScale(); // Diverging cyan, grey, amber over -1..1
    const SKColorScale& hue();    // Cyclic over 0..360

    // A built-in by its id, nullptr for none.
    const SKColorScale* byName(const std::string& id);
    const std::vector<std::string>& names();
}

#endif // SK_COLORSCALE_H
