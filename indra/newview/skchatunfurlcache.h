/**
 * @file skchatunfurlcache.h
 * @brief Previews of the links in local chat, looked up once per link and kept for reuse.
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

#ifndef SK_CHATUNFURLCACHE_H
#define SK_CHATUNFURLCACHE_H

#include "llavatarpropertiesprocessor.h"
#include "llgroupmgr.h"
#include "llpointer.h"
#include "skchatunfurl.h"

#include <list>
#include <memory>
#include <unordered_map>

class LLViewerFetchedTexture;

struct SKChatEmbed
{
    enum EState { IDLE, PENDING, READY, FAILED };

    SKChatUnfurl::Target mTarget;
    std::string mUrl;          // what a click opens, the link as chat has it
    EState mState = IDLE;
    std::string mTitle;
    std::string mDescription;  // one line
    LLPointer<LLViewerFetchedTexture> mImage;
    bool mRoundImage = false;  // a resident's picture
    F64 mDeadline = 0.0;       // a region lookup gives up after this
};
using SKChatEmbedPtr = std::shared_ptr<SKChatEmbed>;

/// Looks up what a chat link previews through the grid's own services, or for a Marketplace
/// listing its page, only for links the allowlist in skchatunfurl.h accepts. Results and
/// failures are kept by canonical link, the least recently used dropped past a limit.
class SKChatUnfurlCache final : public LLAvatarPropertiesObserver, public LLParticularGroupObserver
{
public:
    SKChatUnfurlCache();
    ~SKChatUnfurlCache() override;

    /// The preview for a link, shared by every message with it; null when it is not previewed.
    SKChatEmbedPtr find(const std::string& url);
    /// Starts the lookup of an idle preview.
    void request(const SKChatEmbedPtr& embed);
    /// Advances lookups waiting on the grid; cheap while none are.
    void update();
    /// Counts changes to any preview, so a reader knows to lay out again.
    U32 generation() const { return *mGeneration; }

    void processProperties(void* data, EAvatarProcessorType type) override;
    void changed(const LLUUID& group_id, LLGroupChange gc) override;

private:
    class ParcelLookup;

    void touch(const SKChatEmbedPtr& embed);
    void requestPlace(const SKChatEmbedPtr& embed);
    void requestParcel(const SKChatEmbedPtr& embed, const LLUUID& region_id, const LLVector3& pos, const LLVector3d& global);
    void requestPage(const SKChatEmbedPtr& embed);
    void fillGroup(const LLUUID& group_id);
    void finish(SKChatEmbed& embed, SKChatEmbed::EState state);

    using lru_t = std::list<SKChatEmbedPtr>;
    lru_t mLru; // most recent first
    std::unordered_map<std::string, lru_t::iterator> mByKey;
    std::vector<std::weak_ptr<SKChatEmbed>> mWaitingRegions;
    std::vector<std::unique_ptr<ParcelLookup>> mParcelLookups;
    std::unordered_multimap<LLUUID, std::weak_ptr<SKChatEmbed>> mWaitingAgents;
    std::unordered_multimap<LLUUID, std::weak_ptr<SKChatEmbed>> mWaitingGroups;
    std::shared_ptr<U32> mGeneration;
    F64 mNextPoll = 0.0;
};

#endif // SK_CHATUNFURLCACHE_H
