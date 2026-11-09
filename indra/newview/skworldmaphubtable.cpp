/**
 * @file skworldmaphubtable.cpp
 * @brief The 3D world map's freight hubs: whole regions that serve some ways of travel, read from a hub list page.
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

#include "skworldmaphubtable.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace skworldmap
{
    namespace
    {
        char lower(char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        std::string lowered(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(), lower);
            return out;
        }

        void appendUTF8(std::string& out, U32 cp)
        {
            if (cp < 0x80)
            {
                out += static_cast<char>(cp);
            }
            else if (cp < 0x800)
            {
                out += static_cast<char>(0xc0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3f));
            }
            else if (cp < 0x10000)
            {
                out += static_cast<char>(0xe0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                out += static_cast<char>(0x80 | (cp & 0x3f));
            }
            else if (cp < 0x110000)
            {
                out += static_cast<char>(0xf0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                out += static_cast<char>(0x80 | (cp & 0x3f));
            }
        }

        /// A cell's text: tags dropped, entities decoded, white space collapsed and trimmed.
        std::string cellText(std::string_view raw)
        {
            std::string out;
            out.reserve(raw.size());
            bool space = false;
            auto put = [&](char c)
            {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                {
                    space = !out.empty();
                    return;
                }
                if (space)
                {
                    out += ' ';
                    space = false;
                }
                out += c;
            };
            for (size_t i = 0; i < raw.size(); ++i)
            {
                const char c = raw[i];
                if (c == '<')
                {
                    const size_t close = raw.find('>', i);
                    if (close == std::string_view::npos)
                    {
                        break;
                    }
                    i = close;
                    continue;
                }
                if (c == '&')
                {
                    const size_t semi = raw.find(';', i);
                    if (semi != std::string_view::npos && semi - i <= 10)
                    {
                        const std::string_view name = raw.substr(i + 1, semi - i - 1);
                        std::string decoded;
                        if (name == "amp") decoded = "&";
                        else if (name == "lt") decoded = "<";
                        else if (name == "gt") decoded = ">";
                        else if (name == "quot") decoded = "\"";
                        else if (name == "apos") decoded = "'";
                        else if (name == "nbsp") decoded = " ";
                        else if (name.size() > 1 && name[0] == '#')
                        {
                            const bool hex = name[1] == 'x' || name[1] == 'X';
                            U32 cp = 0;
                            bool ok = name.size() > (hex ? 2u : 1u);
                            for (size_t k = hex ? 2 : 1; ok && k < name.size(); ++k)
                            {
                                const char d = lower(name[k]);
                                if (d >= '0' && d <= '9') cp = cp * (hex ? 16 : 10) + (d - '0');
                                else if (hex && d >= 'a' && d <= 'f') cp = cp * 16 + (d - 'a' + 10);
                                else ok = false;
                                ok = ok && cp < 0x110000;
                            }
                            if (ok)
                            {
                                appendUTF8(decoded, cp);
                            }
                        }
                        if (!decoded.empty())
                        {
                            for (char d : decoded)
                            {
                                put(d);
                            }
                            i = semi;
                            continue;
                        }
                    }
                }
                put(c);
            }
            return out;
        }

        /// Whether `s` at `at` opens the tag `name` (as "<name>" or "<name ...>"), any case.
        bool opensTag(std::string_view s, size_t at, std::string_view name)
        {
            if (at + 1 + name.size() >= s.size() || s[at] != '<')
            {
                return false;
            }
            for (size_t k = 0; k < name.size(); ++k)
            {
                if (lower(s[at + 1 + k]) != name[k])
                {
                    return false;
                }
            }
            const char next = s[at + 1 + name.size()];
            return next == '>' || next == ' ' || next == '\t' || next == '\n' || next == '\r';
        }

        /// The next `<name ...>...</name>` from `from`, as its inner text's span; npos when there is none.
        /// `end` bounds the search, and a cell left open runs to the next cell, row or `end`.
        bool nextElement(std::string_view s, size_t& from, size_t end, std::string_view name, std::string_view& inner)
        {
            for (size_t at = s.find('<', from); at != std::string_view::npos && at < end; at = s.find('<', at + 1))
            {
                if (!opensTag(s, at, name))
                {
                    continue;
                }
                const size_t open_end = s.find('>', at);
                if (open_end == std::string_view::npos || open_end >= end)
                {
                    return false;
                }
                const size_t body = open_end + 1;
                size_t stop = end;
                for (size_t k = s.find('<', body); k != std::string_view::npos && k < end; k = s.find('<', k + 1))
                {
                    if (k + 2 < end && s[k + 1] == '/')
                    {
                        const std::string_view rest = s.substr(k + 2, name.size());
                        if (lowered(rest) == name)
                        {
                            stop = k;
                            break;
                        }
                    }
                    if (opensTag(s, k, name) || (name != "tr" && opensTag(s, k, "tr")))
                    {
                        stop = k;
                        break;
                    }
                }
                inner = s.substr(body, stop - body);
                from = stop;
                return true;
            }
            return false;
        }

        HubSize sizeOf(std::string_view cell)
        {
            if (cell.size() != 1)
            {
                return HubSize::None;
            }
            switch (lower(cell[0]))
            {
                case 's': return HubSize::Small;
                case 'm': return HubSize::Medium;
                case 'l': return HubSize::Large;
                case 'y': return HubSize::Medium;
                default: return HubSize::None;
            }
        }

        constexpr std::string_view SERVICE_HEADINGS[HUB_SERVICES] = { "air", "heli", "water", "seaplane", "road", "rail", "space" };
    } // namespace

    bool Hub::servesAny() const
    {
        return std::any_of(mSizes.begin(), mSizes.end(), [](HubSize s) { return s != HubSize::None; });
    }

    HubTable HubTable::parse(std::string_view html)
    {
        HubTable table;
        constexpr S32 NONE = -1;
        S32 region_col = NONE, name_col = NONE, area_col = NONE;
        std::array<S32, HUB_SERVICES> service_col;
        service_col.fill(NONE);

        size_t pos = 0;
        std::string_view row;
        while (nextElement(html, pos, html.size(), "tr", row))
        {
            size_t cell_pos = 0;
            std::string_view cell;
            if (region_col == NONE)
            {
                // The first row of headings says which column is which.
                S32 col = 0;
                for (; nextElement(row, cell_pos, row.size(), "th", cell); ++col)
                {
                    const std::string heading = lowered(cellText(cell));
                    if (heading == "region") region_col = col;
                    else if (heading == "hub name" || heading == "hub") name_col = col;
                    else if (heading == "area") area_col = col;
                    for (size_t s = 0; s < HUB_SERVICES; ++s)
                    {
                        if (heading == SERVICE_HEADINGS[s])
                        {
                            service_col[s] = col;
                        }
                    }
                }
                continue;
            }

            std::vector<std::string> cells;
            while (nextElement(row, cell_pos, row.size(), "td", cell))
            {
                cells.push_back(cellText(cell));
            }
            if (region_col >= static_cast<S32>(cells.size()) || cells[region_col].empty())
            {
                continue;
            }
            Hub& hub = table.mHubs.emplace_back();
            hub.mRegion = cells[region_col];
            auto at = [&](S32 col) { return col != NONE && col < static_cast<S32>(cells.size()) ? cells[col] : std::string(); };
            hub.mName = at(name_col);
            hub.mArea = at(area_col);
            for (size_t s = 0; s < HUB_SERVICES; ++s)
            {
                hub.mSizes[s] = sizeOf(at(service_col[s]));
            }
        }

        table.mIndex.reserve(table.mHubs.size());
        for (U32 i = 0; i < table.mHubs.size(); ++i)
        {
            table.mIndex.emplace_back(lowered(table.mHubs[i].mRegion), i);
        }
        // A region listed twice keeps its first row.
        std::stable_sort(table.mIndex.begin(), table.mIndex.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        table.mIndex.erase(std::unique(table.mIndex.begin(), table.mIndex.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
                           table.mIndex.end());
        return table;
    }

    const Hub* HubTable::find(std::string_view region) const
    {
        const std::string key = lowered(region);
        auto it = std::lower_bound(mIndex.begin(), mIndex.end(), key, [](const auto& e, const std::string& k) { return e.first < k; });
        return it != mIndex.end() && it->first == key ? &mHubs[it->second] : nullptr;
    }

    HubPlaces HubPlaces::parse(std::string_view tsv)
    {
        HubPlaces places;
        auto number = [](std::string_view field, F32& out)
        {
            const std::string s(field);
            char* end = nullptr;
            const F32 v = std::strtof(s.c_str(), &end);
            if (s.empty() || end != s.c_str() + s.size() || !std::isfinite(v))
            {
                return false;
            }
            out = v;
            return true;
        };
        size_t pos = 0;
        while (pos < tsv.size())
        {
            size_t eol = tsv.find('\n', pos);
            if (eol == std::string_view::npos)
            {
                eol = tsv.size();
            }
            std::string_view line = tsv.substr(pos, eol - pos);
            pos = eol + 1;
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty() || line.front() == '#')
            {
                continue;
            }
            std::vector<std::string_view> fields;
            for (size_t start = 0;;)
            {
                const size_t tab = line.find('\t', start);
                fields.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
                if (tab == std::string_view::npos)
                {
                    break;
                }
                start = tab + 1;
            }
            HubPlace place;
            if (fields.size() < 3 || fields[0].empty() || !number(fields[1], place.mX) || !number(fields[2], place.mY) ||
                (fields.size() > 3 && !fields[3].empty() && !number(fields[3], place.mZ)))
            {
                continue;
            }
            places.mPlaces.emplace_back(lowered(fields[0]), place);
        }
        // A region given twice keeps its first place.
        std::stable_sort(places.mPlaces.begin(), places.mPlaces.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        places.mPlaces.erase(std::unique(places.mPlaces.begin(), places.mPlaces.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
                             places.mPlaces.end());
        return places;
    }

    const HubPlace* HubPlaces::find(std::string_view region) const
    {
        const std::string key = lowered(region);
        auto it = std::lower_bound(mPlaces.begin(), mPlaces.end(), key, [](const auto& e, const std::string& k) { return e.first < k; });
        return it != mPlaces.end() && it->first == key ? &it->second : nullptr;
    }
} // namespace skworldmap
