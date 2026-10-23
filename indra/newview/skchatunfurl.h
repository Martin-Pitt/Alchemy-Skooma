/**
 * @file skchatunfurl.h
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

#ifndef SK_CHATUNFURL_H
#define SK_CHATUNFURL_H

#include "lluuid.h"

#include <string>
#include <string_view>

// Every viewer in chat range would fetch a previewed link, so a link to any other host would
// let its owner log their addresses. Only Linden Lab's own hosts are previewed, and only those
// a viewer talks to anyway; nothing here touches the network.
namespace SKChatUnfurl
{
enum class EKind
{
    NONE,
    PLACE,       // a region and position, resolved through the grid
    PARCEL,      // a parcel by its id
    AGENT,
    GROUP,
    MARKETPLACE, // a listing page, read for its Open Graph tags
};

struct Target
{
    EKind mKind = EKind::NONE;
    std::string mKey;    // canonical, the cache key; for a listing, the page fetched
    std::string mRegion; // PLACE
    F32 mX = 128.f, mY = 128.f, mZ = 0.f;
    LLUUID mID;          // PARCEL, AGENT, GROUP
};

/// What a chat link would preview, NONE for anything not on the allowlist.
Target classify(std::string_view url);

/// The host of an https URL whose authority is a plain host name (no userinfo, port, IP literal,
/// punycode or non-ASCII), lowercased; empty otherwise. path receives the rest from the first '/'.
std::string httpsHost(std::string_view url, std::string* path = nullptr);

/// Hosts whose pages are fetched for previews.
bool isPageHost(std::string_view host);
/// Hosts a preview's image may come from.
bool isImageHost(std::string_view host);

/// A redirect is followed only to the host the request was made to.
bool redirectAllowed(std::string_view from, std::string_view location);

/// An og:image URL on an image host, without query or fragment, with a jpeg or png extension;
/// empty when it may not be shown.
std::string imageUrl(std::string_view url);

struct OpenGraph
{
    std::string mTitle;
    std::string mDescription;
    std::string mImage;
};

constexpr size_t MAX_PAGE_BYTES = 256 * 1024;
constexpr size_t MAX_TITLE_BYTES = 200;
constexpr size_t MAX_DESCRIPTION_BYTES = 400;

/// og:title, og:description (or the description meta) and og:image from the head of a page,
/// read from at most MAX_PAGE_BYTES, entities decoded, control characters dropped.
OpenGraph parseOpenGraph(std::string_view html);
} // namespace SKChatUnfurl

#endif // SK_CHATUNFURL_H
