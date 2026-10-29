/**
 * @file sklegendmodel.h
 * @brief What one legend says: ramps, keys, states, fields, channels and readouts.
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

#ifndef SK_LEGENDMODEL_H
#define SK_LEGENDMODEL_H

#include "skcolorscale.h"
#include "skvizkey.h"

#include <memory>
#include <string>
#include <vector>

// One glyph of a row: what the drawer puts on screen for this class.
struct SKLegendMark
{
    enum class Kind : U8
    {
        Swatch,
        Line,       // at its real width
        Dashed,
        Dot,
        Ring,
        Arrow,
        Hashed,     // a hatched swatch, for a state
        Strip,      // a run of colours
        Cross,
        Point,
        BoxOutline,
        Text,       // a token in the drawer's own letters
        Image       // a skin image by name
    };

    Kind                  kind = Kind::Swatch;
    LLColor4              colour = LLColor4::white;
    F32                   width  = 1.f;
    std::vector<LLColor4> strip;
    std::string           token;

    static SKLegendMark swatch(const LLColor4& c) { return { Kind::Swatch, c }; }
    static SKLegendMark line(const LLColor4& c, F32 w = 1.f) { SKLegendMark m { Kind::Line, c }; m.width = w; return m; }
    static SKLegendMark of(Kind k, const LLColor4& c) { return { k, c }; }
    static SKLegendMark text(const std::string& t, const LLColor4& c) { SKLegendMark m { Kind::Text, c }; m.token = t; return m; }
    static SKLegendMark image(const std::string& name) { SKLegendMark m { Kind::Image }; m.token = name; return m; }
};

// A row: its marks side by side, a label and a right-aligned value.
struct SKLegendRow
{
    std::vector<SKLegendMark> marks;
    std::string               label;
    std::string               value;
    SKVizKey                  key;
};

// A rail over a ramp.
struct SKLegendMarker
{
    enum class Kind : U8
    {
        Selected,
        Threshold,
        Reference
    };
    F64         value = 0.0;
    std::string label;
    Kind        kind = Kind::Threshold;
};

struct SKLegendSection
{
    enum class Type : U8
    {
        Ramp,
        Key,
        States,
        Fields,
        Channels,
        Readout
    };
    // What a ramp's position encodes, colour or a non-colour channel.
    enum class Channel : U8
    {
        Colour,
        Width,
        Length,
        Dash,
        Pattern,
        Height
    };
    enum class Shape : U8
    {
        Bar,
        Ring
    };
    // What a key's colour is drawn on.
    enum class KeyChannel : U8
    {
        Fill,
        Outline,
        Line
    };

    Type        type = Type::Key;
    std::string heading;

    // Ramp: the owner's scale, shared rather than copied.
    std::shared_ptr<const SKColorScale> scale;
    std::string                         quantity;
    std::string                         unit; // "" is dimensionless
    Channel                             channel = Channel::Colour;
    Shape                               shape   = Shape::Bar;
    std::vector<SKLegendMarker>         markers;

    // Key / States / Fields / Channels / Readout.
    KeyChannel               keyChannel = KeyChannel::Fill;
    std::string              format; // Fields: how the label reads, "prio / area"
    std::vector<SKLegendRow> rows;

    // A ramp over a scale the caller keeps alive (a built-in, a member); a scale on the stack would dangle.
    static SKLegendSection ramp(const SKColorScale& s, std::string quantity, std::string unit = std::string())
    {
        return ramp(std::shared_ptr<const SKColorScale>(&s, [](const SKColorScale*) {}), std::move(quantity), std::move(unit));
    }
    static SKLegendSection ramp(std::shared_ptr<const SKColorScale> s, std::string quantity, std::string unit = std::string())
    {
        SKLegendSection sec;
        sec.type     = Type::Ramp;
        sec.scale    = std::move(s);
        sec.quantity = std::move(quantity);
        sec.unit     = unit.empty() && sec.scale ? sec.scale->unit() : std::move(unit);
        return sec;
    }
    // A key whose rows are a discrete scale's classes, keyed by class index.
    static SKLegendSection key(const SKColorScale& s, std::string heading = std::string());
    static SKLegendSection make(Type type, std::string heading, std::vector<SKLegendRow> rows)
    {
        SKLegendSection sec;
        sec.type    = type;
        sec.heading = std::move(heading);
        sec.rows    = std::move(rows);
        return sec;
    }
};

struct SKLegend
{
    std::string                  title;
    std::string                  subtitle;
    std::string                  note;
    std::vector<SKLegendSection> sections;

    bool empty() const { return title.empty() && sections.empty(); }
};

#endif // SK_LEGENDMODEL_H
