/**
 * @file skchatunfurl.cpp
 * @brief Which links in local chat may be previewed, and reading a preview from a page.
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

#include "skchatunfurl.h"

#include <algorithm>
#include <cstdlib>

namespace SKChatUnfurl
{
namespace
{
constexpr std::string_view MAPS_HOST = "maps.secondlife.com";
constexpr std::string_view WORLD_HOST = "world.secondlife.com";
constexpr std::string_view MARKETPLACE_HOST = "marketplace.secondlife.com";
constexpr std::string_view MARKETPLACE_IMAGE_HOST = "slm-assets.secondlife.com";
constexpr size_t MAX_REGION_NAME = 64;

char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool iequals_prefix(std::string_view s, std::string_view prefix)
{
    if (s.size() < prefix.size())
    {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        if (lower(s[i]) != lower(prefix[i]))
        {
            return false;
        }
    }
    return true;
}

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && iequals_prefix(a, b);
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Splits a path into its segments, ignoring a query or fragment.
size_t split_path(std::string_view path, std::string_view* out, size_t max)
{
    path = path.substr(0, std::min(path.find('?'), path.find('#')));
    size_t count = 0;
    while (!path.empty())
    {
        if (path.front() == '/')
        {
            path.remove_prefix(1);
            continue;
        }
        const size_t end = std::min(path.find('/'), path.size());
        if (count == max)
        {
            return max + 1;
        }
        out[count++] = path.substr(0, end);
        path.remove_prefix(end);
    }
    return count;
}

bool percent_decode(std::string_view in, std::string& out)
{
    out.clear();
    for (size_t i = 0; i < in.size(); ++i)
    {
        char c = in[i];
        if (c == '%')
        {
            if (i + 2 >= in.size())
            {
                return false;
            }
            const int hi = hex_value(in[i + 1]);
            const int lo = hex_value(in[i + 2]);
            if (hi < 0 || lo < 0)
            {
                return false;
            }
            c = (char)(hi * 16 + lo);
            i += 2;
        }
        if ((unsigned char)c < 0x20 || c == 0x7f)
        {
            return false;
        }
        out += c;
    }
    return true;
}

bool parse_uuid(std::string_view s, LLUUID& id)
{
    if (s.size() != UUID_STR_LENGTH - 1)
    {
        return false;
    }
    const std::string str(s);
    if (!LLUUID::validate(str))
    {
        return false;
    }
    id.set(str);
    return id.notNull();
}

bool parse_coord(std::string_view s, F32 max, F32& value)
{
    if (s.empty() || s.size() > 8)
    {
        return false;
    }
    for (char c : s)
    {
        if (!(c >= '0' && c <= '9') && c != '.' && c != '-')
        {
            return false;
        }
    }
    value = llclamp((F32)std::atof(std::string(s).c_str()), 0.f, max);
    return true;
}

// Region and optional x/y/z, as both SLURL forms carry them.
Target place(const std::string_view* seg, size_t count)
{
    Target target;
    std::string region;
    if (count < 1 || count > 4 || !percent_decode(seg[0], region) || region.empty() || region.size() > MAX_REGION_NAME)
    {
        return target;
    }
    F32 xyz[3] = { 128.f, 128.f, 0.f };
    const F32 limits[3] = { 256.f, 256.f, 4096.f };
    for (size_t i = 1; i < count; ++i)
    {
        if (!parse_coord(seg[i], limits[i - 1], xyz[i - 1]))
        {
            return target;
        }
    }
    target.mKind = EKind::PLACE;
    target.mRegion = region;
    target.mX = xyz[0];
    target.mY = xyz[1];
    target.mZ = xyz[2];
    std::string key = "place:";
    for (char c : region)
    {
        key += lower(c);
    }
    key += llformat("/%d/%d/%d", (S32)xyz[0], (S32)xyz[1], (S32)xyz[2]);
    target.mKey = std::move(key);
    return target;
}

Target by_id(EKind kind, std::string_view id_text, std::string_view key_prefix)
{
    Target target;
    if (parse_uuid(id_text, target.mID))
    {
        target.mKind = kind;
        target.mKey = std::string(key_prefix) + target.mID.asString();
    }
    return target;
}

// An entity's numeric or named reference, decoded; anything else is dropped.
std::string decode_entities(std::string_view in, size_t max_bytes)
{
    std::string out;
    out.reserve(std::min(in.size(), max_bytes));
    bool space = false;
    for (size_t i = 0; i < in.size() && out.size() < max_bytes; ++i)
    {
        U32 cp = (unsigned char)in[i];
        if (in[i] == '&')
        {
            const size_t semi = in.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 10)
            {
                const std::string_view name = in.substr(i + 1, semi - i - 1);
                cp = 0;
                if (name == "amp") cp = '&';
                else if (name == "lt") cp = '<';
                else if (name == "gt") cp = '>';
                else if (name == "quot") cp = '"';
                else if (name == "apos") cp = '\'';
                else if (name == "nbsp") cp = ' ';
                else if (name.size() > 1 && name[0] == '#')
                {
                    const bool hex = name[1] == 'x' || name[1] == 'X';
                    const std::string digits(name.substr(hex ? 2 : 1));
                    char* end = nullptr;
                    const unsigned long v = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
                    if (!digits.empty() && end && *end == 0 && v < 0x110000 && !(v >= 0xD800 && v < 0xE000))
                    {
                        cp = (U32)v;
                    }
                }
                i = semi;
                if (cp == 0)
                {
                    continue;
                }
                if (cp >= 0x80)
                {
                    // UTF-8 for the code point.
                    char buf[4];
                    size_t n = 0;
                    if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
                    else if (cp < 0x10000) { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
                    else { buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
                    if (out.size() + n + (space ? 1 : 0) > max_bytes)
                    {
                        break;
                    }
                    if (space)
                    {
                        out += ' ';
                    }
                    out.append(buf, n);
                    space = false;
                    continue;
                }
            }
        }
        // Runs of whitespace and control characters become one space.
        if (cp < 0x20 || cp == ' ' || cp == 0x7f)
        {
            space = !out.empty();
            continue;
        }
        if (space)
        {
            out += ' ';
            space = false;
            if (out.size() >= max_bytes)
            {
                break;
            }
        }
        out += (char)cp;
    }
    // Never end inside a UTF-8 sequence.
    size_t cut = out.size();
    size_t back = 0;
    while (cut > 0 && back < 4 && ((unsigned char)out[cut - 1] & 0xC0) == 0x80)
    {
        --cut;
        ++back;
    }
    if (cut > 0 && (unsigned char)out[cut - 1] >= 0xC0)
    {
        const unsigned char lead = (unsigned char)out[cut - 1];
        const size_t need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : 1;
        if (back < need)
        {
            out.resize(cut - 1);
        }
    }
    return out;
}

// The value of one attribute of a tag's text, empty when absent.
std::string_view attribute(std::string_view tag, std::string_view name)
{
    size_t pos = 0;
    while (pos < tag.size())
    {
        // Skip to an attribute name.
        while (pos < tag.size() && (tag[pos] == ' ' || tag[pos] == '\t' || tag[pos] == '\n' || tag[pos] == '\r' || tag[pos] == '/'))
        {
            ++pos;
        }
        const size_t start = pos;
        while (pos < tag.size() && tag[pos] != '=' && tag[pos] != ' ' && tag[pos] != '\t' && tag[pos] != '\n' && tag[pos] != '\r' && tag[pos] != '/')
        {
            ++pos;
        }
        const std::string_view attr = tag.substr(start, pos - start);
        if (pos >= tag.size() || tag[pos] != '=')
        {
            if (pos == start)
            {
                ++pos;
            }
            continue;
        }
        ++pos;
        std::string_view value;
        if (pos < tag.size() && (tag[pos] == '"' || tag[pos] == '\''))
        {
            const char quote = tag[pos++];
            const size_t end = tag.find(quote, pos);
            if (end == std::string_view::npos)
            {
                return {};
            }
            value = tag.substr(pos, end - pos);
            pos = end + 1;
        }
        else
        {
            const size_t begin = pos;
            while (pos < tag.size() && tag[pos] != ' ' && tag[pos] != '\t' && tag[pos] != '\n' && tag[pos] != '\r')
            {
                ++pos;
            }
            value = tag.substr(begin, pos - begin);
        }
        if (iequals(attr, name))
        {
            return value;
        }
    }
    return {};
}
} // namespace

std::string httpsHost(std::string_view url, std::string* path)
{
    constexpr std::string_view scheme = "https://";
    if (!iequals_prefix(url, scheme))
    {
        return {};
    }
    url.remove_prefix(scheme.size());
    const size_t end = std::min(url.find_first_of("/?#"), url.size());
    const std::string_view authority = url.substr(0, end);
    if (authority.empty() || authority.size() > 253 || authority.back() == '.' || authority.front() == '.')
    {
        return {};
    }
    std::string host;
    host.reserve(authority.size());
    bool all_numeric = true;
    size_t label = 0;
    for (size_t i = 0; i < authority.size(); ++i)
    {
        const char c = lower(authority[i]);
        // Userinfo, ports, IPv6 literals, backslashes and anything outside plain ASCII names are refused.
        const bool letter = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (!(letter || digit || c == '-' || c == '.'))
        {
            return {};
        }
        if (c == '.')
        {
            if (label == 0)
            {
                return {};
            }
            label = 0;
        }
        else
        {
            // Punycode labels could spell a lookalike.
            if (label == 0 && authority.substr(i).size() >= 4 && iequals_prefix(authority.substr(i), "xn--"))
            {
                return {};
            }
            ++label;
        }
        all_numeric &= digit || c == '.';
        host += c;
    }
    if (all_numeric)
    {
        return {}; // an IPv4 literal
    }
    if (path)
    {
        *path = std::string(url.substr(end));
    }
    return host;
}

bool isPageHost(std::string_view host)
{
    return host == MARKETPLACE_HOST;
}

bool isImageHost(std::string_view host)
{
    return host == MARKETPLACE_IMAGE_HOST;
}

bool redirectAllowed(std::string_view from, std::string_view location)
{
    const std::string from_host = httpsHost(from);
    const std::string to_host = httpsHost(location);
    return !from_host.empty() && from_host == to_host && isPageHost(to_host);
}

std::string imageUrl(std::string_view url)
{
    std::string path;
    const std::string host = httpsHost(url, &path);
    if (!isImageHost(host))
    {
        return {};
    }
    path.resize(std::min(path.find_first_of("?#"), path.size()));
    std::string lowered;
    for (char c : path)
    {
        lowered += lower(c);
    }
    auto ends_with = [&](std::string_view ext)
    {
        return lowered.size() > ext.size() && lowered.compare(lowered.size() - ext.size(), ext.size(), ext) == 0;
    };
    if (path.size() < 2 || !(ends_with(".jpg") || ends_with(".jpeg") || ends_with(".png")))
    {
        return {};
    }
    for (char c : path)
    {
        if ((unsigned char)c <= 0x20 || (unsigned char)c >= 0x7f || c == '\\')
        {
            return {};
        }
    }
    return "https://" + host + path;
}

Target classify(std::string_view url)
{
    constexpr size_t MAX_SEGMENTS = 8;
    std::string_view seg[MAX_SEGMENTS];

    constexpr std::string_view app = "secondlife:///app/";
    if (iequals_prefix(url, app))
    {
        const size_t count = split_path(url.substr(app.size()), seg, MAX_SEGMENTS);
        if (count > MAX_SEGMENTS || count < 2)
        {
            return {};
        }
        if (count == 3 && iequals(seg[2], "about"))
        {
            if (iequals(seg[0], "agent"))
            {
                return by_id(EKind::AGENT, seg[1], "agent:");
            }
            if (iequals(seg[0], "group"))
            {
                return by_id(EKind::GROUP, seg[1], "group:");
            }
        }
        if (iequals(seg[0], "teleport") || iequals(seg[0], "worldmap"))
        {
            return place(seg + 1, count - 1);
        }
        return {};
    }

    std::string path;
    const std::string host = httpsHost(url, &path);
    if (host.empty())
    {
        return {};
    }
    const size_t count = split_path(path, seg, MAX_SEGMENTS);
    if (count > MAX_SEGMENTS)
    {
        return {};
    }
    if (host == MAPS_HOST)
    {
        if (count >= 2 && iequals(seg[0], "secondlife"))
        {
            return place(seg + 1, count - 1);
        }
        return {};
    }
    if (host == WORLD_HOST)
    {
        // The web profiles name their subject by id, so the grid answers for them, no page is read.
        if (count == 2)
        {
            if (iequals(seg[0], "resident")) return by_id(EKind::AGENT, seg[1], "agent:");
            if (iequals(seg[0], "group")) return by_id(EKind::GROUP, seg[1], "group:");
            if (iequals(seg[0], "place")) return by_id(EKind::PARCEL, seg[1], "parcel:");
        }
        return {};
    }
    if (host == MARKETPLACE_HOST)
    {
        if (count != 3 || seg[0] != "p" || seg[1].empty() || seg[1].size() > 200 || seg[2].empty() || seg[2].size() > 12)
        {
            return {};
        }
        for (char c : seg[2])
        {
            if (c < '0' || c > '9')
            {
                return {};
            }
        }
        for (char c : seg[1])
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '%' || c == '.';
            if (!ok)
            {
                return {};
            }
        }
        Target target;
        target.mKind = EKind::MARKETPLACE;
        target.mKey = "https://marketplace.secondlife.com/p/" + std::string(seg[1]) + "/" + std::string(seg[2]);
        return target;
    }
    return {};
}

OpenGraph parseOpenGraph(std::string_view html)
{
    OpenGraph og;
    html = html.substr(0, std::min(html.size(), MAX_PAGE_BYTES));
    std::string description_meta;
    size_t pos = 0;
    while (pos < html.size())
    {
        const size_t open = html.find('<', pos);
        if (open == std::string_view::npos)
        {
            break;
        }
        const size_t close = html.find('>', open);
        if (close == std::string_view::npos)
        {
            break;
        }
        const std::string_view tag = html.substr(open + 1, close - open - 1);
        pos = close + 1;
        if (iequals_prefix(tag, "/head") || iequals_prefix(tag, "body"))
        {
            break;
        }
        if (!iequals_prefix(tag, "meta") || tag.size() < 5 || !(tag[4] == ' ' || tag[4] == '\t' || tag[4] == '\n' || tag[4] == '\r'))
        {
            continue;
        }
        const std::string_view body = tag.substr(5);
        std::string_view key = attribute(body, "property");
        if (key.empty())
        {
            key = attribute(body, "name");
        }
        const std::string_view content = attribute(body, "content");
        if (content.empty())
        {
            continue;
        }
        if (iequals(key, "og:title") && og.mTitle.empty())
        {
            og.mTitle = decode_entities(content, MAX_TITLE_BYTES);
        }
        else if (iequals(key, "og:description") && og.mDescription.empty())
        {
            og.mDescription = decode_entities(content, MAX_DESCRIPTION_BYTES);
        }
        else if (iequals(key, "description") && description_meta.empty())
        {
            description_meta = decode_entities(content, MAX_DESCRIPTION_BYTES);
        }
        else if (iequals(key, "og:image") && og.mImage.empty())
        {
            og.mImage = decode_entities(content, 2048);
        }
    }
    if (og.mDescription.empty())
    {
        og.mDescription = std::move(description_meta);
    }
    return og;
}
} // namespace SKChatUnfurl
