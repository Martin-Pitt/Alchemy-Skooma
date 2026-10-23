/**
 * @file skchatunfurl_test.cpp
 * @brief Tests for which chat links may be previewed, and reading a preview from a page.
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

#include "../skchatunfurl.h"
#include <catch2/catch_test_macros.hpp>

using namespace SKChatUnfurl;

namespace
{
const std::string AGENT = "a2e76fcd-9360-4f6d-a924-000000000003";

TEST_CASE("skchatunfurl: listing pages are accepted and made canonical", "[skchatunfurl]")
{
    const Target t = classify("https://marketplace.secondlife.com/p/Cumulus-Broom-Rezzer/27534733?ple=c#reviews");
    REQUIRE(t.mKind == EKind::MARKETPLACE);
    REQUIRE(t.mKey == "https://marketplace.secondlife.com/p/Cumulus-Broom-Rezzer/27534733");
    REQUIRE(classify("HTTPS://Marketplace.SecondLife.com/p/x/1").mKind == EKind::MARKETPLACE);
}

TEST_CASE("skchatunfurl: anything but an exact allowlisted https host is refused", "[skchatunfurl]")
{
    for (const char* url : {
             "http://marketplace.secondlife.com/p/x/1",                 // not https
             "https://marketplace.secondlife.com.evil.com/p/x/1",       // suffix lookalike
             "https://evil.com/marketplace.secondlife.com/p/x/1",
             "https://evilsecondlife.com/p/x/1",
             "https://marketplace.secondIife.com/p/x/1",                // capital I for l
             "https://xn--marketplace-secondlife-abc.com/p/x/1",        // punycode
             "https://marketplace.xn--secondlfe-c7a.com/p/x/1",
             "https://user@marketplace.secondlife.com/p/x/1",           // userinfo
             "https://marketplace.secondlife.com@evil.com/p/x/1",
             "https://evil.com\\@marketplace.secondlife.com/p/x/1",
             "https://marketplace.secondlife.com:8443/p/x/1",           // port
             "https://marketplace.secondlife.com:443/p/x/1",
             "https://marketplace.secondlife.com./p/x/1",               // trailing dot
             "https://127.0.0.1/p/x/1",                                 // IP literals
             "https://[::1]/p/x/1",
             "https://3232235777/p/x/1",
             "https://marketpl\xc3\xa0" "ce.secondlife.com/p/x/1",      // non-ASCII
             "https://secondlife.com/destination/somewhere",            // no og tags, not fetched
             "https://slm-assets.secondlife.com/assets/1/thumbnail/a.jpg", // an image host is no page
             "ftp://marketplace.secondlife.com/p/x/1",
             "javascript:alert(1)",
             "",
         })
    {
        CAPTURE(url);
        REQUIRE(classify(url).mKind == EKind::NONE);
    }
}

TEST_CASE("skchatunfurl: only listing paths on the marketplace", "[skchatunfurl]")
{
    for (const char* url : {
             "https://marketplace.secondlife.com/",
             "https://marketplace.secondlife.com/products/search?q=x",
             "https://marketplace.secondlife.com/p/x/abc",
             "https://marketplace.secondlife.com/p/x/1/extra",
             "https://marketplace.secondlife.com/p/x\"y/1",
         })
    {
        CAPTURE(url);
        REQUIRE(classify(url).mKind == EKind::NONE);
    }
}

TEST_CASE("skchatunfurl: SLURLs name a place without any page", "[skchatunfurl]")
{
    Target t = classify("https://maps.secondlife.com/secondlife/Da%20Boom/128/64/22");
    REQUIRE(t.mKind == EKind::PLACE);
    REQUIRE(t.mRegion == "Da Boom");
    REQUIRE(t.mX == 128.f);
    REQUIRE(t.mY == 64.f);
    REQUIRE(t.mZ == 22.f);
    REQUIRE(t.mKey == "place:da boom/128/64/22");

    t = classify("secondlife:///app/teleport/Da%20Boom/128/64/22");
    REQUIRE(t.mKind == EKind::PLACE);
    REQUIRE(t.mKey == "place:da boom/128/64/22");
    REQUIRE(classify("secondlife:///app/worldmap/Ahern").mKind == EKind::PLACE);
    REQUIRE(classify("https://maps.secondlife.com/secondlife/Ahern/999/-5/1").mX == 256.f);

    REQUIRE(classify("http://maps.secondlife.com/secondlife/Ahern/1/2/3").mKind == EKind::NONE);
    REQUIRE(classify("https://maps.secondlife.com/other/Ahern/1/2/3").mKind == EKind::NONE);
    REQUIRE(classify("https://maps.secondlife.com/secondlife/Bad%0AName/1/2/3").mKind == EKind::NONE);
    REQUIRE(classify("https://maps.secondlife.com/secondlife/Ahern/x/2/3").mKind == EKind::NONE);
    REQUIRE(classify("secondlife:///app/teleport/").mKind == EKind::NONE);
}

TEST_CASE("skchatunfurl: residents and groups by their about links and web profiles", "[skchatunfurl]")
{
    Target t = classify("secondlife:///app/agent/" + AGENT + "/about");
    REQUIRE(t.mKind == EKind::AGENT);
    REQUIRE(t.mID.asString() == AGENT);
    REQUIRE(t.mKey == "agent:" + AGENT);
    REQUIRE(classify("https://world.secondlife.com/resident/" + AGENT).mKey == t.mKey);

    REQUIRE(classify("secondlife:///app/group/" + AGENT + "/about").mKind == EKind::GROUP);
    REQUIRE(classify("https://world.secondlife.com/group/" + AGENT).mKind == EKind::GROUP);
    REQUIRE(classify("https://world.secondlife.com/place/" + AGENT).mKind == EKind::PARCEL);

    // Name links and other actions are not previews.
    REQUIRE(classify("secondlife:///app/agent/" + AGENT + "/inspect").mKind == EKind::NONE);
    REQUIRE(classify("secondlife:///app/agent/" + AGENT + "/completename").mKind == EKind::NONE);
    REQUIRE(classify("secondlife:///app/agent/not-a-uuid/about").mKind == EKind::NONE);
    REQUIRE(classify("secondlife:///app/agent/00000000-0000-0000-0000-000000000000/about").mKind == EKind::NONE);
    REQUIRE(classify("http://world.secondlife.com/resident/" + AGENT).mKind == EKind::NONE);
}

TEST_CASE("skchatunfurl: redirects are followed only on the same page host", "[skchatunfurl]")
{
    const char* from = "https://marketplace.secondlife.com/p/x/1";
    REQUIRE(redirectAllowed(from, "https://marketplace.secondlife.com/p/y/1"));
    REQUIRE_FALSE(redirectAllowed(from, "https://id.secondlife.com/openid/checklogin"));
    REQUIRE_FALSE(redirectAllowed(from, "https://evil.com/"));
    REQUIRE_FALSE(redirectAllowed(from, "http://marketplace.secondlife.com/p/y/1"));
    REQUIRE_FALSE(redirectAllowed(from, "https://marketplace.secondlife.com:8080/"));
    REQUIRE_FALSE(redirectAllowed(from, "/p/y/1"));
}

TEST_CASE("skchatunfurl: images come only from the listing image host", "[skchatunfurl]")
{
    REQUIRE(imageUrl("https://slm-assets.secondlife.com/assets/37502308/thumbnail/TTS%20SWEATER%20DEMO.jpg?1758495291") ==
            "https://slm-assets.secondlife.com/assets/37502308/thumbnail/TTS%20SWEATER%20DEMO.jpg");
    REQUIRE(!imageUrl("https://slm-assets.secondlife.com/a/b.PNG").empty());
    REQUIRE(imageUrl("https://evil.com/a.jpg").empty());
    REQUIRE(imageUrl("http://slm-assets.secondlife.com/a.jpg").empty());
    REQUIRE(imageUrl("https://slm-assets.secondlife.com.evil.com/a.jpg").empty());
    REQUIRE(imageUrl("https://marketplace.secondlife.com/a.jpg").empty());
    REQUIRE(imageUrl("https://slm-assets.secondlife.com/a.svg").empty());
    REQUIRE(imageUrl("https://slm-assets.secondlife.com/a b.jpg").empty());
}

TEST_CASE("skchatunfurl: Open Graph tags are read from the head alone", "[skchatunfurl]")
{
    const std::string html =
        "<!DOCTYPE html><html><head><title>x</title>"
        "<meta name=\"description\" content=\"Fallback\">"
        "<meta property=\"og:title\" content=\"MANTRA :: TTS &amp; &#x27;DEMO&#39; &#233;\"/>"
        "<meta content='https://slm-assets.secondlife.com/a.jpg' property='og:image'>"
        "</head><body><meta property=\"og:description\" content=\"in the body\"></body></html>";
    const OpenGraph og = parseOpenGraph(html);
    REQUIRE(og.mTitle == "MANTRA :: TTS & 'DEMO' \xc3\xa9");
    REQUIRE(og.mImage == "https://slm-assets.secondlife.com/a.jpg");
    REQUIRE(og.mDescription == "Fallback");
}

TEST_CASE("skchatunfurl: page text is bounded and cleaned", "[skchatunfurl]")
{
    const std::string long_title(5000, 'a');
    OpenGraph og = parseOpenGraph("<meta property=\"og:title\" content=\"" + long_title + "\">"
                                  "<meta property=\"og:description\" content=\"one\n\ttwo\x01  three\">");
    REQUIRE(og.mTitle.size() == MAX_TITLE_BYTES);
    REQUIRE(og.mDescription == "one two three");

    // A tag past the cap is never seen.
    std::string padded(MAX_PAGE_BYTES, ' ');
    padded += "<meta property=\"og:title\" content=\"late\">";
    REQUIRE(parseOpenGraph(padded).mTitle.empty());

    // Unterminated tags and quotes end the scan quietly.
    REQUIRE(parseOpenGraph("<meta property=\"og:title\" content=\"x").mTitle.empty());
    REQUIRE(parseOpenGraph("<meta property=\"og:title\" content=\"x>").mTitle.empty());
    REQUIRE(parseOpenGraph("").mTitle.empty());
}
} // namespace
