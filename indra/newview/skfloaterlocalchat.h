/**
 * @file skfloaterlocalchat.h
 * @brief Game-style nearby chat: messages over the world that fade, with the input shown only while chatting.
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

#ifndef SK_FLOATERLOCALCHAT_H
#define SK_FLOATERLOCALCHAT_H

#include "llfloater.h"
#include "llchat.h"
#include "llfontgl.h"
#include "llrendertarget.h"
#include "skchatunfurlcache.h"

#include <deque>

class LLButton;
class LLChatEntry;
class LLFontGL;
class LLMenuButton;

class SKFloaterLocalChat final : public LLFloater
{
public:
    AL_VIEW_TYPE(SKFloaterLocalChat, LLFloater);

    SKFloaterLocalChat(const LLSD& key);
    ~SKFloaterLocalChat() override;

    /// Whether this floater replaces the nearby toasts and chat bar (SkoomaLocalChatModern).
    static bool modernEnabled();
    /// Whether starting chat opens this floater: modern chat is on and Conversations is not up to take it.
    static bool shouldTakeChat();
    /// Takes a nearby chat message as the toasts would have.
    static void addChat(const LLChat& chat, const LLSD& args);
    /// Shows the input with the cursor at the end, starting it with line when given.
    static void startChat(const char* line);
    static void stopChat();
    /// The text being typed, empty when the floater does not exist.
    static std::string currentChat();

    bool postBuild() override;
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    bool applyRectControl() override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    void onMouseEnter(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void onFocusLost() override;

private:
    enum EStyle : S32 { STYLE_CARDS = 0, STYLE_MINIMAL = 1 };

    struct Link
    {
        S32 mBegin = 0; // bytes into the body
        S32 mEnd = 0;   // exclusive
        std::string mUrl;
        bool mQuery = false; // a web link's query, drawn grey as chat history does
    };

    struct Message
    {
        std::string mBody;  // what is drawn; may start with the speaker's name
        std::string mName;  // the card header
        std::string mSenderUrl;
        LLUUID mFromID;
        LLColor4 mTextColor;
        LLColor4 mNameColor;
        S32 mNameLen = 0;   // bytes of the name at the start of mBody, 0 for none
        EChatSourceType mSource = CHAT_SOURCE_SYSTEM;
        bool mEmote = false;
        F64 mArrival = 0.0;
        std::string mTime;  // the timestamp after the name
        S32 mNameWidth = 0; // measured at layout
        S32 mTimeWidth = 0; // the timestamp and its gap, 0 for none
        std::vector<Link> mLinks;
        SKChatEmbedPtr mEmbed; // the preview of the first link that has one
        S32 mEmbedWidth = 0;   // measured at layout
    };

    struct Row
    {
        S32 mMessage = 0;   // -1 for a card header
        S32 mCard = 0;
        S32 mBegin = 0;
        S32 mLength = 0;
        S32 mPadTop = 0;
        S32 mPadBottom = 0;
        S32 mGap = 0;       // space below the row, outside its card
        S32 mBottom = 0;    // from the bottom of the content
        bool mEmbed = false; // the message's link preview rather than a line of it
        S32 height() const;
    };

    struct Card
    {
        S32 mFirstMessage = 0;
        S32 mLastMessage = 0;
        S32 mFirstRow = 0;
        S32 mLastRow = 0;
        S32 mWidth = 0;     // the bubble, shrink-wrapped to its widest line
    };

    void backfill(bool skip_last);
    void appendChat(const LLChat& chat, const LLSD& args, F64 arrival);
    void showInput(bool show);
    void sendChat(EChatType type);
    void invalidateLayout() { mLayoutDirty = true; }
    void layout();
    void layoutCog();
    LLRect contentRect() const;
    bool expanded() const;
    F32 cardAlpha(const Card& card, F64 now) const;
    S32 lineHeight(const Row& row) const;
    /// The url under a point in the floater, empty when none.
    std::string urlAt(S32 x, S32 y) const;
    /// The visible cards in UI units, unclipped and unmasked.
    void drawMessages(const LLRect& content, F64 now, bool full, bool cards) const;
    /// The visible cards, faded out at the edges through mMask when any reach into the edge bands.
    void drawMaskedMessages(const LLRect& content, F64 now, bool full, bool cards);
    void drawTime(const Message& msg, F32 x, S32 y, F32 alpha, LLFontGL::ShadowType shadow) const;
    void drawRowText(const Row& row, S32 x, S32 y, F32 alpha, LLFontGL::ShadowType shadow) const;
    void drawEmbed(const Message& msg, S32 x, S32 y, F32 alpha) const;
    /// Starts the lookups of the previews in a card about to be drawn.
    void requestEmbeds(const Card& card);
    void onStyleMenu(const LLSD& param);
    bool onStyleCheck(const LLSD& param) const;

    LLChatEntry* mInput = nullptr;
    LLMenuButton* mCog = nullptr;
    LLButton* mEmojiButton = nullptr;
    U32 mEmojiClosedFrame = 0;  // the frame the picker last closed, to tell a toggle-off click
    boost::signals2::connection mEmojiCloseConnection;
    std::deque<Message> mMessages;
    std::vector<Card> mCards;
    std::vector<Row> mRows;
    S32 mContentHeight = 0;
    S32 mScroll = 0;            // pixels scrolled up from the newest message
    S32 mPendingMessages = 0;   // appended since the last layout
    S32 mUnseen = 0;            // arrived while scrolled up
    S32 mPillCount = -1;        // the count mPillLabel was made for
    std::string mPillLabel;
    LLRect mPillRect;           // empty while the jump pill is hidden
    S32 mLayoutWidth = -1;
    S32 mLayoutStyle = -1;
    bool mLayoutDirty = true;
    bool mHovered = false;
    bool mWasExpanded = false;
    F64 mCollapsedAt = -1.0e9; // when the log last stopped being shown whole
    boost::signals2::connection mFontConnection;

    struct MaskKey
    {
        U32 mWidth = 0;
        U32 mHeight = 0;
        S32 mScroll = -1;
        U32 mLayout = 0;
        U64 mFades = 0;
        bool mCards = false;
        bool operator==(const MaskKey&) const = default;
    };
    LLRenderTarget mMask;       // the messages, premultiplied, for the scroll mask
    MaskKey mMaskKey;           // what mMask holds
    U32 mLayoutGeneration = 0;  // counts layouts, so mMask knows the rows moved
    SKChatUnfurlCache mUnfurl;
    U32 mUnfurlGeneration = 0;  // the cache's generation the layout was made for
};

#endif // SK_FLOATERLOCALCHAT_H
