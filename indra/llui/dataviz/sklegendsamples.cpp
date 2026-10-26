/**
 * @file sklegendsamples.cpp
 * @brief Hand-built legends that exercise every part of the model, for the demo floater and the tests.
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

#include "sklegendsamples.h"

namespace
{
    LLColor4 rgb(F32 r, F32 g, F32 b) { return LLColor4(r, g, b, 1.f); }

    SKLegendRow row(std::vector<SKLegendMark> marks, std::string label, std::string value, const std::string& scale, U32 category)
    {
        SKLegendRow r;
        r.marks        = std::move(marks);
        r.label        = std::move(label);
        r.value        = std::move(value);
        r.key.scale    = scale;
        r.key.category = category;
        return r;
    }

    SKLegendRow field(const std::string& token, std::string meaning, std::string unit = std::string())
    {
        SKLegendRow r;
        r.marks.push_back(SKLegendMark::text(token, rgb(.95f, .95f, .30f)));
        r.label = std::move(meaning);
        r.value = std::move(unit);
        return r;
    }
}

namespace SKLegendSamples
{
    std::shared_ptr<SKColorScale> avatarComplexityScale()
    {
        auto s = std::make_shared<SKColorScale>(
            SKColorScale::even("avatar-complexity", SKColorScale::Kind::Sequential, { rgb(.30f, 1.f, .45f), rgb(.95f, .95f, .30f), rgb(1.f, .30f, .30f) }));
        s->setDomain(0.0, 350000.0).setOver(rgb(1.f, .30f, .85f));
        return s;
    }

    SKLegend avatarDrawInfo(const std::shared_ptr<const SKColorScale>& complexity)
    {
        SKLegend l;
        l.title    = "Avatar draw info";
        SKLegendSection ramp = SKLegendSection::ramp(complexity, "complexity");
        ramp.markers.push_back({ complexity->hi() * 0.5, "half the limit", SKLegendMarker::Kind::Threshold });
        l.sections.push_back(ramp);
        SKLegendSection f = SKLegendSection::make(SKLegendSection::Type::Fields, "fields", {
            field("C", "complexity"),
            field("A", "attachment surface", "m2"),
            field("H", "height", "m"),
            field("ms", "render time", "ms"),
        });
        f.format = "C / A / H / ms";
        l.sections.push_back(f);
        l.note = "magenta: over the complexity limit";
        return l;
    }

    SKLegend updateType()
    {
        SKLegend l;
        l.title = "Update type";
        const std::string s = "update-type";
        l.sections.push_back(SKLegendSection::make(SKLegendSection::Type::Key, "last update", {
            row({ SKLegendMark::swatch(rgb(1.f, .30f, .30f)) }, "full", "", s, 0),
            row({ SKLegendMark::swatch(rgb(.30f, 1.f, .45f)) }, "terse", "", s, 1),
            row({ SKLegendMark::swatch(rgb(.20f, .90f, 1.f)) }, "cached", "", s, 2),
            row({ SKLegendMark::swatch(rgb(.95f, .95f, .30f)) }, "compressed", "", s, 3),
        }));
        SKLegendRow none;
        none.marks.push_back(SKLegendMark::of(SKLegendMark::Kind::Hashed, rgb(.55f, .55f, .60f)));
        none.label     = "no update yet";
        none.key.scale = s;
        none.key.state = "none";
        l.sections.push_back(SKLegendSection::make(SKLegendSection::Type::States, "", { none }));
        return l;
    }

    SKLegend lodInfo()
    {
        SKLegend l;
        l.title = "LOD info";
        SKLegendSection f = SKLegendSection::make(SKLegendSection::Type::Fields, "", {
            field("0", "lowest"),
            field("1", "low"),
            field("2", "medium"),
            field("3", "high"),
        });
        f.format = "level over each face";
        l.sections.push_back(f);
        return l;
    }

    SKLegend bufferView()
    {
        SKLegend l;
        l.title    = "Buffer view 1";
        l.subtitle = "G-buffer specular / ORM";
        l.sections.push_back(SKLegendSection::make(SKLegendSection::Type::Channels, "", {
            row({ SKLegendMark::swatch(rgb(1.f, 0.f, 0.f)) }, "occlusion", "R", "", 0),
            row({ SKLegendMark::swatch(rgb(0.f, 1.f, 0.f)) }, "roughness", "G", "", 1),
            row({ SKLegendMark::swatch(rgb(0.f, 0.f, 1.f)) }, "metallic", "B", "", 2),
            row({ SKLegendMark::swatch(rgb(.6f, .6f, .6f)) }, "unused", "A", "", 3),
        }));
        SKLegendRow lum;
        lum.label = "mean luminance";
        lum.value = "0.42";
        l.sections.push_back(SKLegendSection::make(SKLegendSection::Type::Readout, "", { lum }));
        return l;
    }

    SKLegend nameTagDistance()
    {
        static const SKColorScale bins = [] {
            SKColorScale s = SKColorScale::even("name-tag-distance", SKColorScale::Kind::Binned,
                { rgb(.30f, 1.f, .45f), rgb(.95f, .95f, .30f), rgb(1.f, .70f, .20f), rgb(1.f, .30f, .30f) });
            s.setEdges({ 0.0, 10.0, 20.0, 64.0, 96.0 }).setUnit("m");
            return s;
        }();
        SKLegend l;
        l.title = "Name tag distance";
        l.sections.push_back(SKLegendSection::ramp(bins, "distance"));
        l.sections.push_back(SKLegendSection::key(bins));
        return l;
    }

    SKLegend beacons()
    {
        SKLegend l;
        l.title = "Beacons";
        const std::string s = "beacons";
        l.sections.push_back(SKLegendSection::make(SKLegendSection::Type::Key, "", {
            row({ SKLegendMark::line(rgb(1.f, .30f, .30f), 2.f) }, "scripted", "", s, 0),
            row({ SKLegendMark::line(rgb(.20f, .90f, 1.f), 2.f) }, "scripted with touch", "", s, 1),
            row({ SKLegendMark::line(rgb(.30f, 1.f, .45f), 2.f) }, "physical", "", s, 2),
            row({ SKLegendMark::line(rgb(.95f, .95f, .30f), 2.f) }, "sound source", "", s, 3),
            row({ SKLegendMark::of(SKLegendMark::Kind::Dashed, rgb(.70f, .45f, 1.f)) }, "particle source", "", s, 4),
            row({ SKLegendMark::of(SKLegendMark::Kind::Ring, rgb(1.f, .70f, .20f)), SKLegendMark::of(SKLegendMark::Kind::Cross, rgb(1.f, .70f, .20f)) },
                "media", "", s, 5),
        }));
        return l;
    }

    SKLegend builtInScales()
    {
        SKLegend l;
        l.title = "Built-in scales";
        for (const std::string& name : SKColorScales::names())
        {
            const SKColorScale& s = *SKColorScales::byName(name);
            if (s.isDiscrete())
            {
                l.sections.push_back(SKLegendSection::key(s, name));
            }
            else
            {
                SKLegendSection r = SKLegendSection::ramp(s, name);
                if (s.kind() == SKColorScale::Kind::Cyclic)
                {
                    SKLegendSection ring = SKLegendSection::ramp(s, name + " (ring)");
                    ring.shape           = SKLegendSection::Shape::Ring;
                    l.sections.push_back(r);
                    l.sections.push_back(ring);
                    continue;
                }
                l.sections.push_back(r);
            }
        }
        SKLegendSection width = SKLegendSection::ramp(SKColorScales::ink(), "width channel");
        width.channel         = SKLegendSection::Channel::Width;
        l.sections.push_back(width);
        SKLegendSection dash = SKLegendSection::ramp(SKColorScales::ink(), "dash channel");
        dash.channel         = SKLegendSection::Channel::Dash;
        l.sections.push_back(dash);
        return l;
    }
}
