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
#include "llavatarpropertiesprocessor.h"
#include "llpointer.h"
#include "skoutlinecapture.h"
#include "llrendertarget.h"
#include "skchatunfurlcache.h"

#include <deque>

class LLButton;
class LLChatEntry;
class LLFontGL;
class LLMenuButton;
class LLViewerFetchedTexture;

class SKFloaterLocalChat final : public LLFloater, public LLAvatarPropertiesObserver
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
    /// Collapsed, the chat is a compact bar at its bottom left with the unread count and the typing line.
    void setMinimized(bool minimize) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    void onMouseEnter(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void onFocusLost() override;
    void processProperties(void* data, EAvatarProcessorType type) override;

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
        S32 mDay = 0;          // yyyymmdd in the timestamps' time base, 0 when unknown
        bool mHistory = false; // loaded from the chat log rather than heard this session
        std::string mDayLabel; // the day divider above it, made at layout when needed
        S32 mDayWidth = 0;     // measured at layout
    };

    struct Row
    {
        S32 mMessage = 0;   // -1 for a card header, DIVIDER_ROW for a day divider
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
        S32 mIconWidth = 0; // the speaker's icon, 0 for none
        S32 mTextX = 0;     // where the text column starts, from the content's left
        bool mDivider = false; // a day divider, fading with the message after it
    };
    static constexpr S32 DIVIDER_ROW = -2;

    void backfill(bool skip_last);
    /// day is the message's yyyymmdd when the caller knows it better than its timestamp does, else 0.
    void appendChat(const LLChat& chat, const LLSD& args, F64 arrival, S32 day = 0);
    void showInput(bool show);
    void sendChat(EChatType type);
    void invalidateLayout() { mLayoutDirty = true; }
    S32 inputHeight() const;
    void onInputResized();
    void layout();
    void layoutCog();
    LLRect contentRect() const;
    bool barShown() const;
    LLRect barStrip() const;
    LLRect barThumb() const;    // the thumb's whole extent in the strip
    S32 barRange() const;
    void barPage(S32 dir);
    bool barTakes(S32 x, S32 y); // a click or hover here belongs to the bar
    bool expanded() const;
    F32 cardAlpha(const Card& card, F64 now) const;
    S32 lineHeight(const Row& row) const;
    /// The url under a point in the floater, empty when none.
    std::string urlAt(S32 x, S32 y) const;
    /// The visible cards in UI units, unclipped and unmasked.
    void drawMessages(const LLRect& content, F64 now, bool full, bool cards);
    /// The visible cards, faded out at the edges through mMask when any reach into the edge bands.
    void drawMaskedMessages(const LLRect& content, F64 now, bool full, bool cards);
    void drawTime(const Message& msg, F32 x, S32 y, F32 alpha, LLFontGL::ShadowType shadow) const;
    void drawRowText(const Row& row, S32 x, S32 y, F32 alpha, LLFontGL::ShadowType shadow) const;
    /// An object speaker's side-view glyph, null until the capture has one.
    const SKOutlineLayer* objectGlyph(const LLUUID& speaker, F64 now);
    /// An icon's width at the card icon height: an object's glyph keeps its aspect, everything else is square.
    S32 iconWidth(const Message& first, F64 now);
    void drawObjectIcon(const Message& first, S32 left, S32 top, S32 width, const LLColor4& color, F64 now);
    U64 iconStamp(const Message& first) const;
    void drawAvatarIcon(const Message& first, S32 left, S32 top, const LLColor4& color);
    void drawDivider(const Card& card, const LLRect& content, S32 base, F32 alpha, LLFontGL::ShadowType shadow) const;
    void drawEmbed(const Message& msg, S32 x, S32 y, F32 alpha) const;
    /// Starts the lookups of the previews in a card about to be drawn.
    void requestEmbeds(const Card& card);
    /// Polls who nearby is typing, a few times a second, and remakes the label when that changes.
    void updateTyping(F64 now);
    void makeTypingLabel(S32 group);
    void drawTyping(F64 now, S32 y) const;
    bool anyoneTyping() const { return !mTypingLabel[0].empty() || !mTypingLabel[1].empty(); }
    /// Sizes the collapsed bar to its label, and to the typing line while anyone types.
    void layoutCollapsed();
    void drawCollapsed(F64 now);
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
    bool mBarHot = false;       // the pointer is on the scroll bar's strip
    S32 mBarDrag = -1;          // while the thumb is dragged, the pointer's height above its bottom
    S32 mBarPage = 0;           // while the track is held, +1 paging back through history, -1 forward
    S32 mBarPointerY = 0;       // the pointer's last height, so a held page stops at it
    F64 mBarRepeatAt = 0.0;     // when a held page next repeats
    F32 mBarWide = 0.f;         // 0 for the thin thumb, 1 for the full bar, eased between
    F64 mBarDrawnAt = 0.0;      // the last draw, to ease mBarWide by
    F32 mBackdrop = 0.f;        // the hover tint, 0 to 1, eased in and out
    F32 mBorder = 0.f;          // the edge border, likewise
    F64 mEdgeSince = -1.0;      // when the pointer reached the floater's edge, -1 while it is elsewhere
    F64 mChromeDrawnAt = 0.0;
    S32 mCollapsedUnread = 0;   // arrived since the chat was collapsed
    std::string mCollapsedLabel;
    bool mWasExpanded = false;
    F64 mCollapsedAt = -1.0e9; // when the log last stopped being shown whole
    boost::signals2::connection mFontConnection;

    // An object speaker's linkset root and the base layer of its glyph, looked at a few times a second.
    struct ObjectGlyph
    {
        F64 mChecked = -1.0e9;
        bool mShown = false;
        SKOutlineLayer mLayer;
    };
    std::unordered_map<LLUUID, ObjectGlyph> mObjectGlyphs;

    // A person's profile picture, through the icon id cache the other chats share.
    struct AvatarPicture
    {
        LLPointer<LLViewerFetchedTexture> mTexture;
        bool mAsked = false;     // the cache or the properties processor has been asked
        bool mObserving = false; // waiting on a properties reply
    };
    std::unordered_map<LLUUID, AvatarPicture> mAvatarPictures;
    struct MaskKey
    {
        U32 mWidth = 0;
        U32 mHeight = 0;
        S32 mScroll = -1;
        U32 mLayout = 0;
        U64 mFades = 0;
        U64 mIcons = 0;
        bool mCards = false;
        bool operator==(const MaskKey&) const = default;
    };
    LLRenderTarget mMask;       // the messages, premultiplied, for the scroll mask
    MaskKey mMaskKey;           // what mMask holds
    U32 mLayoutGeneration = 0;  // counts layouts, so mMask knows the rows moved
    struct Typer
    {
        LLUUID mID;
        S32 mMetres = 0;   // 0 within chat range, else the whole metres beyond it
        S32 mDistance = 0; // whole metres away, for the bracketed form
        F64 mTypedAt = 0.0; // last seen typing, for the out-of-range grace
        bool operator==(const Typer& o) const { return mID == o.mID && mMetres == o.mMetres && mDistance == o.mDistance; }
        bool operator<(const Typer& o) const { return mID < o.mID; }
    };
    std::vector<Typer> mTyping[2];    // within chat range and just beyond it, sorted
    std::vector<Typer> mTypingScratch[2];
    std::string mTypingLabel[2];
    std::string mTypingFarCount;      // the out-of-range part as a count, for when the line is short
    F64 mTypingPollAt = 0.0;
    bool mTypingNamed[2] = { true, true }; // every name in the label had resolved
    SKChatUnfurlCache mUnfurl;
    U32 mUnfurlGeneration = 0;  // the cache's generation the layout was made for
};

#endif // SK_FLOATERLOCALCHAT_H
