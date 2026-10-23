/**
 * @file skfloaterlocalchat.cpp
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

#include "llviewerprecompiledheaders.h"

#include "skfloaterlocalchat.h"

#include "llautoreplace.h"
#include "llbutton.h"
#include "llchatentry.h"
#include "llemojihelper.h"
#include "lldbstrings.h"
#include "lldraghandle.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "lllocalcliprect.h"
#include "llmenubutton.h"
#include "llrender2dutils.h"
#include "llslurl.h"
#include "lltrans.h"
#include "lluicolortable.h"
#include "lluiimage.h"
#include "llurlaction.h"
#include "llurlmatch.h"
#include "llurlregistry.h"
#include "llwindow.h"

#include "alavatargroups.h"
#include "alchatbar.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llfloaterimnearbychat.h"
#include "llviewerchat.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

namespace
{
constexpr size_t MAX_MESSAGES = 300;
constexpr S32 MAX_CARD_MESSAGES = 8;
constexpr F64 CARD_JOIN_SECONDS = 60.0;
constexpr S32 PAD_X = 8;
constexpr S32 PAD_Y = 5;
constexpr S32 CARD_GAP = 4;
constexpr S32 MINIMAL_GAP = 2;
constexpr S32 DISC = 12;
constexpr S32 DISC_GAP = 6;
constexpr S32 INPUT_HEIGHT = 24;
constexpr S32 INPUT_GAP = 4;
constexpr S32 INSET = 8; // inside the backdrop's border
constexpr F32 EDGE_FADE = 24.f; // the scroll mask at the top, and at the bottom while scrolled
constexpr F32 CARD_ALPHA = 0.6f;

S32 utf8_lead_len(char c)
{
    const U8 b = (U8)c;
    return b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
}

LLFontGL* name_font()
{
    return LLFontGL::getFontSansSerifSmall();
}
} // namespace

S32 SKFloaterLocalChat::Row::height() const
{
    // Padding and gap only; the line itself depends on the font, see lineHeight().
    return mPadTop + mPadBottom + mGap;
}

SKFloaterLocalChat::SKFloaterLocalChat(const LLSD& key)
    : LLFloater(key)
{
    mCommitCallbackRegistrar.add("SKLocalChat.Style", [this](LLUICtrl*, const LLSD& param) { onStyleMenu(param); });
    mEnableCallbackRegistrar.add("SKLocalChat.CheckStyle", [this](LLUICtrl*, const LLSD& param) { return onStyleCheck(param); });
}

SKFloaterLocalChat::~SKFloaterLocalChat()
{
    mFontConnection.disconnect();
    mEmojiCloseConnection.disconnect();
}

// static
bool SKFloaterLocalChat::modernEnabled()
{
    static LLCachedControl<bool> modern(gSavedSettings, "SkoomaLocalChatModern", true);
    return modern;
}

// static
void SKFloaterLocalChat::addChat(const LLChat& chat, const LLSD& args)
{
    SKFloaterLocalChat* self = LLFloaterReg::findTypedInstance<SKFloaterLocalChat>("sk_local_chat");
    if (!self)
    {
        self = LLFloaterReg::getTypedInstance<SKFloaterLocalChat>("sk_local_chat");
        if (!self)
        {
            return;
        }
        // The archive already holds this message; take it from here instead, with its args.
        self->backfill(true);
    }
    self->appendChat(chat, args, LLFrameTimer::getTotalSeconds());
    if (!self->getVisible())
    {
        LLFloaterReg::showInstance("sk_local_chat", LLSD(), false);
    }
}

// static
void SKFloaterLocalChat::startChat(const char* line)
{
    SKFloaterLocalChat* self = LLFloaterReg::findTypedInstance<SKFloaterLocalChat>("sk_local_chat");
    if (!self)
    {
        self = LLFloaterReg::getTypedInstance<SKFloaterLocalChat>("sk_local_chat");
        if (!self)
        {
            return;
        }
        self->backfill(false);
    }
    if (!self->getVisible())
    {
        LLFloaterReg::showInstance("sk_local_chat", LLSD(), false);
    }
    self->showInput(true);
    if (line)
    {
        self->mInput->setText(LLStringExplicit(line));
    }
    // Always at the end, so a stray WASD does not overwrite what was typed.
    self->mInput->endOfDoc();
}

// static
void SKFloaterLocalChat::stopChat()
{
    if (SKFloaterLocalChat* self = LLFloaterReg::findTypedInstance<SKFloaterLocalChat>("sk_local_chat"))
    {
        self->showInput(false);
    }
    gAgent.stopTyping();
}

// static
std::string SKFloaterLocalChat::currentChat()
{
    SKFloaterLocalChat* self = LLFloaterReg::findTypedInstance<SKFloaterLocalChat>("sk_local_chat");
    return self && self->mInput ? self->mInput->getText() : LLStringUtil::null;
}

bool SKFloaterLocalChat::postBuild()
{
    mInput = getChild<LLChatEntry>("sk_local_chat_input");
    mInput->setAutoreplaceCallback(boost::bind(&LLAutoReplace::autoreplaceCallback, LLAutoReplace::getInstance(), _1, _2, _3, _4, _5));
    mInput->setKeystrokeCallback([this](LLTextEditor*) { ALChatBar::handleInputKeystroke(mInput, [this]() { sendChat(CHAT_TYPE_NORMAL); }); });
    mInput->setFocusLostCallback([](LLFocusableElement*) { gAgent.stopTyping(); });
    mInput->setCommitOnFocusLost(false);
    mInput->setPassDelete(true);
    mInput->setShowChatMentionPicker(true);
    mInput->setShowEmojiHelper(true);
    mInput->enableSingleLineMode(true);
    mInput->setMaxTextLength(DB_CHAT_MSG_STR_LEN * 5);
    mInput->setFont(LLViewerChat::getChatFont());
    ALChatBar::applyChannelLabel(mInput, gSavedSettings.getS32("AlchemyNearbyChatChannel"));
    mInput->setVisible(false);

    // The emoji picker as the conversation window has it; the toggle state follows the picker.
    mEmojiButton = getChild<LLButton>("sk_local_chat_emoji");
    mEmojiButton->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        mInput->hideEmojiHelper();
        if (!mEmojiButton->getToggleState())
        {
            mInput->setFocus(true);
            mInput->showEmojiHelper();
        }
        mEmojiButton->setToggleState(!mEmojiButton->getToggleState());
    });
    mEmojiButton->setMouseDownCallback([this](LLUICtrl*, const LLSD&)
    {
        // The press itself closed the picker; keep the state so the click does not reopen it.
        if (mEmojiClosedFrame == LLFrameTimer::getFrameCount())
        {
            mEmojiButton->setToggleState(true);
        }
    });
    mEmojiCloseConnection = LLEmojiHelper::instance().setCloseCallback([this](LLUICtrl*, const LLSD&)
    {
        if (mEmojiButton->getToggleState())
        {
            mEmojiButton->setToggleState(false);
            mEmojiClosedFrame = LLFrameTimer::getFrameCount();
        }
    });

    mCog = getChild<LLMenuButton>("sk_local_chat_cog");
    mCog->setVisible(false);

    mFontConnection = LLViewerChat::setFontChangedCallback([this](LLFontGL* font)
    {
        mInput->setFont(font);
        invalidateLayout();
    });

    sendChildToBack(getDragHandle());
    layoutCog();
    layoutInput();
    gFloaterView->adjustToFitScreen(this, false);
    return true;
}

void SKFloaterLocalChat::backfill(bool skip_last)
{
    LLFloaterIMNearbyChat* nearby = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
    if (!nearby)
    {
        return;
    }
    const std::vector<LLChat>& archive = nearby->getMessageArchive();
    size_t count = archive.size();
    if (skip_last && count > 0)
    {
        --count;
    }
    for (size_t i = count > MAX_MESSAGES ? count - MAX_MESSAGES : 0; i < count; ++i)
    {
        // Arrival 0 leaves history faded until the chat is opened.
        appendChat(archive[i], LLSD(), 0.0);
    }
}

void SKFloaterLocalChat::appendChat(const LLChat& chat, const LLSD& args, F64 arrival)
{
    if (chat.mText.empty())
    {
        return;
    }

    Message msg;
    msg.mFromID = chat.mFromID;
    msg.mSource = chat.mSourceType;
    msg.mArrival = arrival;

    LLUIColor text_color = LLUIColorTable::instance().getColor("White");
    F32 alpha = 1.f;
    LLViewerChat::getChatColor(chat, text_color, alpha);
    msg.mTextColor = text_color.get();
    msg.mTextColor.mV[VALPHA] *= alpha;
    msg.mNameColor = LLUIColorTable::instance().getColor("ChatHeaderDisplayNameColor").get();
    LLColor4 irc_name;
    if (ALAvatarGroups::instance().getChatNameColor(chat, irc_name))
    {
        msg.mNameColor = irc_name;
    }

    const bool named = !chat.mFromName.empty() && chat.mSourceType != CHAT_SOURCE_SYSTEM;
    if (named && !chat.mRlvNamesFiltered)
    {
        if (chat.mSourceType == CHAT_SOURCE_AGENT && chat.mFromID.notNull())
        {
            msg.mSenderUrl = LLSLURL("agent", chat.mFromID, "inspect").getSLURLString();
        }
        else if (chat.mSourceType == CHAT_SOURCE_OBJECT)
        {
            msg.mSenderUrl = LLViewerChat::getSenderSLURL(chat, args);
        }
    }

    // As chat history: an IRC-style emote runs on from the name, shout and whisper carry their own verb.
    std::string text = chat.mText;
    msg.mEmote = chat.mChatStyle == CHAT_STYLE_IRC;
    if (msg.mEmote && text.size() >= 3)
    {
        text = text.substr(3);
    }
    static LLCachedControl<S32> style(gSavedSettings, "SkoomaLocalChatStyle", STYLE_CARDS);
    if (named)
    {
        msg.mName = chat.mFromName;
        if (msg.mEmote || style == STYLE_MINIMAL)
        {
            static const std::string shout = LLTrans::getString("shout");
            static const std::string whisper = LLTrans::getString("whisper");
            std::string delimiter = ": ";
            if (msg.mEmote || chat.mChatType == CHAT_TYPE_SHOUT || chat.mChatType == CHAT_TYPE_WHISPER ||
                text.compare(0, shout.length(), shout) == 0 || text.compare(0, whisper.length(), whisper) == 0)
            {
                delimiter = msg.mEmote ? "" : " ";
            }
            msg.mBody = chat.mFromName + delimiter;
            msg.mNameLen = (S32)chat.mFromName.size();
        }
    }
    msg.mBody += text;

    // Links, shown by their labels.
    std::string out;
    out.reserve(msg.mBody.size());
    const S32 name_len = msg.mNameLen;
    out.append(msg.mBody, 0, name_len);
    std::string rest = msg.mBody.substr(name_len);
    LLUrlMatch match;
    while (!rest.empty() && LLUrlRegistry::instance().findUrl(rest, match))
    {
        out.append(rest, 0, match.getStart());
        std::string label = match.getLabel();
        if (label.empty())
        {
            label = rest.substr(match.getStart(), match.getEnd() - match.getStart() + 1);
        }
        Link link;
        link.mBegin = (S32)out.size();
        out += label;
        link.mEnd = (S32)out.size();
        link.mUrl = match.getUrl();
        msg.mLinks.push_back(std::move(link));
        rest.erase(0, match.getEnd() + 1);
    }
    out += rest;
    msg.mBody = std::move(out);

    mMessages.push_back(std::move(msg));
    ++mPendingMessages;
    if (mScroll > 0)
    {
        ++mUnseen;
    }
    while (mMessages.size() > MAX_MESSAGES)
    {
        mMessages.pop_front();
    }
    invalidateLayout();
}

void SKFloaterLocalChat::showInput(bool show)
{
    if (!mInput)
    {
        return;
    }
    mInput->setVisible(show);
    mEmojiButton->setVisible(show);
    if (!show)
    {
        mInput->hideEmojiHelper();
    }
    if (show)
    {
        setFocus(true);
        mInput->setFocus(true);
    }
    else
    {
        mInput->setFocus(false);
        mScroll = 0;
    }
}

void SKFloaterLocalChat::sendChat(EChatType type)
{
    ALChatBar::sendChatText(mInput, type);
    gAgent.stopTyping();
    if (gSavedSettings.getBOOL("CloseChatBarOnReturn"))
    {
        stopChat();
    }
}

bool SKFloaterLocalChat::handleKeyHere(KEY key, MASK mask)
{
    if (mInput && mInput->getVisible())
    {
        // The chat bar's modifiers: ctrl shouts, ctrl+shift whispers, alt speaks out of character.
        if (KEY_RETURN == key)
        {
            if (mask == (MASK_CONTROL | MASK_SHIFT))
            {
                sendChat(CHAT_TYPE_WHISPER);
                return true;
            }
            if (mask == MASK_CONTROL)
            {
                sendChat(CHAT_TYPE_SHOUT);
                return true;
            }
            if (mask == MASK_ALT)
            {
                sendChat(CHAT_TYPE_OOC);
                return true;
            }
            if (mask == MASK_NONE)
            {
                sendChat(CHAT_TYPE_NORMAL);
                return true;
            }
        }
        else if (KEY_ESCAPE == key)
        {
            stopChat();
            return true;
        }
    }
    return LLFloater::handleKeyHere(key, mask);
}

void SKFloaterLocalChat::onFocusLost()
{
    // The emoji picker takes focus while it is open; the chat stays open for its pick.
    if (mInput && mInput->getVisible() && !LLEmojiHelper::instance().isActive(mInput))
    {
        stopChat();
    }
    LLFloater::onFocusLost();
}

bool SKFloaterLocalChat::applyRectControl()
{
    if (LLFloater::applyRectControl())
    {
        return true;
    }
    // First open: an MMO-sized box at the bottom left, above the toolbar.
    constexpr S32 DEFAULT_W = 600;
    constexpr S32 DEFAULT_H = 560;
    constexpr S32 MARGIN = 10;
    const LLRect snap = gFloaterView->getSnapRect();
    reshape(DEFAULT_W, DEFAULT_H);
    setOrigin(snap.mLeft + MARGIN, snap.mBottom + MARGIN);
    storeRectControl();
    return true;
}

void SKFloaterLocalChat::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLFloater::reshape(width, height, called_from_parent);
    layoutCog();
    layoutInput();
}

void SKFloaterLocalChat::layoutCog()
{
    if (!mCog)
    {
        return;
    }
    constexpr S32 COG_PX = 16;
    const S32 right = getRect().getWidth() - 4;
    const S32 top = getRect().getHeight() - 2;
    mCog->setRect(LLRect(right - COG_PX, top, right, top - COG_PX));
}

void SKFloaterLocalChat::layoutInput()
{
    // The input row sits on the bottom inset at any size, the emoji button at its right.
    constexpr S32 EMOJI_PX = 20;
    const S32 right = getRect().getWidth() - INSET;
    if (mEmojiButton)
    {
        const S32 bottom = INSET + (INPUT_HEIGHT - EMOJI_PX) / 2;
        mEmojiButton->setRect(LLRect(right - EMOJI_PX, bottom + EMOJI_PX, right, bottom));
    }
    if (mInput)
    {
        mInput->setRect(LLRect(INSET, INSET + INPUT_HEIGHT, right - EMOJI_PX - 4, INSET));
    }
}

void SKFloaterLocalChat::onMouseEnter(S32 x, S32 y, MASK mask)
{
    mHovered = true;
    LLFloater::onMouseEnter(x, y, mask);
}

void SKFloaterLocalChat::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHovered = false;
    LLFloater::onMouseLeave(x, y, mask);
}

bool SKFloaterLocalChat::expanded() const
{
    return mHovered || mScroll > 0 || (mInput && mInput->getVisible()) || gFocusMgr.getMouseCapture() == this;
}

LLRect SKFloaterLocalChat::contentRect() const
{
    // The input's row is always kept, so the messages do not jump when it opens or closes.
    return LLRect(INSET, getRect().getHeight() - INSET, getRect().getWidth() - INSET, INPUT_HEIGHT + INPUT_GAP + INSET);
}

S32 SKFloaterLocalChat::lineHeight(const Row& row) const
{
    return row.mMessage < 0 ? name_font()->getLineHeight() + 1 : LLViewerChat::getChatFont()->getLineHeight();
}

F32 SKFloaterLocalChat::cardAlpha(const Card& card, F64 now) const
{
    static LLCachedControl<F32> delay(gSavedSettings, "SkoomaLocalChatFadeDelay", 8.f);
    static LLCachedControl<F32> fade(gSavedSettings, "SkoomaLocalChatFadeTime", 2.f);
    static LLCachedControl<F32> grace(gSavedSettings, "SkoomaLocalChatHoverGrace", 5.f);
    auto fading = [&](F64 age, F32 hold)
    {
        if (age <= hold)
        {
            return 1.f;
        }
        return fade <= 0.f ? 0.f : llclamp(1.f - (F32)((age - hold) / fade), 0.f, 1.f);
    };
    // After the log was shown whole it stays whole for the grace, then fades as one, as MMO chat does.
    return llmax(fading(now - mMessages[card.mLastMessage].mArrival, delay), fading(now - mCollapsedAt, grace));
}

void SKFloaterLocalChat::layout()
{
    static LLCachedControl<S32> style(gSavedSettings, "SkoomaLocalChatStyle", STYLE_CARDS);
    const bool cards = style != STYLE_MINIMAL;
    const S32 width = contentRect().getWidth();
    if (!mLayoutDirty && width == mLayoutWidth && style == mLayoutStyle)
    {
        return;
    }
    if (mLayoutStyle >= 0 && style != mLayoutStyle)
    {
        // The minimal style folds the name into the body, so the messages are made again.
        mLayoutStyle = style;
        std::deque<Message> old;
        old.swap(mMessages);
        for (Message& msg : old)
        {
            if (!msg.mName.empty() && !msg.mEmote)
            {
                // Strip or add the "Name: " lead the style puts on the body.
                if (msg.mNameLen > 0)
                {
                    const size_t lead = msg.mBody.find_first_not_of(": ", msg.mNameLen);
                    const S32 cut = (S32)(lead == std::string::npos ? msg.mBody.size() : lead);
                    msg.mBody.erase(0, cut);
                    for (Link& link : msg.mLinks)
                    {
                        link.mBegin -= cut;
                        link.mEnd -= cut;
                    }
                    msg.mNameLen = 0;
                }
                if (!cards)
                {
                    const std::string lead = msg.mName + ": ";
                    msg.mBody.insert(0, lead);
                    for (Link& link : msg.mLinks)
                    {
                        link.mBegin += (S32)lead.size();
                        link.mEnd += (S32)lead.size();
                    }
                    msg.mNameLen = (S32)msg.mName.size();
                }
            }
            mMessages.push_back(std::move(msg));
        }
    }
    mLayoutDirty = false;
    mLayoutWidth = width;
    mLayoutStyle = style;
    mRows.clear();
    mCards.clear();

    LLFontGL* font = LLViewerChat::getChatFont();
    const S32 text_x = cards ? PAD_X + DISC + DISC_GAP : PAD_X;
    const F32 wrap = (F32)llmax(16, width - text_x - PAD_X);

    for (S32 m = 0; m < (S32)mMessages.size(); ++m)
    {
        const Message& msg = mMessages[m];
        // Cards gather one speaker's run of messages; a minimal line stands alone.
        bool join = false;
        if (cards && !mCards.empty() && !msg.mName.empty())
        {
            const Card& last = mCards.back();
            const Message& prev = mMessages[last.mLastMessage];
            join = prev.mFromID == msg.mFromID && prev.mSource == msg.mSource && !prev.mName.empty() &&
                   msg.mArrival - prev.mArrival < CARD_JOIN_SECONDS && last.mLastMessage - last.mFirstMessage + 1 < MAX_CARD_MESSAGES;
        }
        if (join)
        {
            mRows.back().mPadBottom = 0;
            mRows.back().mGap = 0;
            mCards.back().mLastMessage = m;
        }
        else
        {
            Card card;
            card.mFirstMessage = card.mLastMessage = m;
            card.mFirstRow = (S32)mRows.size();
            mCards.push_back(card);
            if (cards && !msg.mName.empty())
            {
                Row header;
                header.mMessage = -1;
                header.mCard = (S32)mCards.size() - 1;
                header.mPadTop = PAD_Y;
                mRows.push_back(header);
            }
        }

        const std::string_view body(msg.mBody);
        const S32 total = (S32)body.size();
        S32 pos = 0;
        bool first = true;
        while (pos < total || first)
        {
            S32 line_end = total;
            const size_t nl = body.find('\n', pos);
            if (nl != std::string_view::npos)
            {
                line_end = (S32)nl;
            }
            S32 count = font->maxDrawableBytes(body.substr(pos), wrap, line_end - pos, LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
            if (count <= 0 && pos < line_end)
            {
                count = llmin(utf8_lead_len(body[pos]), line_end - pos);
            }
            Row row;
            row.mMessage = m;
            row.mCard = (S32)mCards.size() - 1;
            row.mBegin = pos;
            row.mLength = count;
            if (first && !join && !(cards && !msg.mName.empty()))
            {
                row.mPadTop = cards ? PAD_Y : 0;
            }
            mRows.push_back(row);
            pos += count;
            if (pos == line_end && line_end < total)
            {
                ++pos; // the newline
            }
            first = false;
            if (count == 0)
            {
                break;
            }
        }
        mRows.back().mPadBottom = cards ? PAD_Y : 0;
        mRows.back().mGap = cards ? CARD_GAP : MINIMAL_GAP;
        mCards.back().mLastRow = (S32)mRows.size() - 1;
    }

    // Each bubble as wide as its name or widest line, wrapping only past the floater's width.
    for (Card& card : mCards)
    {
        S32 widest = 0;
        for (S32 r = card.mFirstRow; r <= card.mLastRow; ++r)
        {
            const Row& row = mRows[r];
            widest = llmax(widest, row.mMessage < 0 ? name_font()->getWidth(mMessages[mCards[row.mCard].mFirstMessage].mName)
                                                    : font->getWidthBytes(mMessages[row.mMessage].mBody, row.mBegin, row.mLength));
        }
        // A little slack for the bold name run, measured above in the regular face.
        card.mWidth = llmin(width, text_x + widest + PAD_X + 2);
    }

    // Offsets from the bottom, newest first.
    S32 y = 0;
    for (S32 r = (S32)mRows.size() - 1; r >= 0; --r)
    {
        mRows[r].mBottom = y;
        y += mRows[r].height() + lineHeight(mRows[r]);
    }
    mContentHeight = y;

    // While scrolled up, what arrived grows the content below the view; scroll by as much so the view holds still.
    const S32 first_new = (S32)mMessages.size() - mPendingMessages;
    mPendingMessages = 0;
    if (mScroll > 0 && first_new > 0)
    {
        for (S32 r = 1; r < (S32)mRows.size(); ++r)
        {
            const Row& row = mRows[r];
            const S32 msg = row.mMessage >= 0 ? row.mMessage : mCards[row.mCard].mFirstMessage;
            if (msg >= first_new)
            {
                mScroll += mRows[r - 1].mBottom;
                break;
            }
        }
    }
}

void SKFloaterLocalChat::drawRowText(const Row& row, S32 x, S32 y, F32 alpha, LLFontGL::ShadowType shadow) const
{
    const Message& msg = mMessages[row.mMessage];
    static LLUIColor link_color = LLUIColorTable::instance().getColor("HTMLLinkColor");
    LLFontGL* font = LLViewerChat::getChatFont();
    const std::string_view body(msg.mBody);
    const S32 end = row.mBegin + row.mLength;
    F32 pen = (F32)x;
    S32 pos = row.mBegin;
    while (pos < end)
    {
        // The run up to the next change of colour: the name, a link, or plain text.
        LLColor4 color = msg.mTextColor;
        S32 run_end = end;
        U8 style = LLFontGL::NORMAL;
        if (pos < msg.mNameLen)
        {
            color = msg.mNameColor;
            run_end = llmin(end, msg.mNameLen);
            style = LLFontGL::BOLD;
        }
        else
        {
            for (const Link& link : msg.mLinks)
            {
                if (pos >= link.mBegin && pos < link.mEnd)
                {
                    color = link_color.get();
                    run_end = llmin(end, link.mEnd);
                    style = LLFontGL::UNDERLINE;
                    break;
                }
                if (link.mBegin > pos && link.mBegin < run_end)
                {
                    run_end = link.mBegin;
                }
            }
        }
        color.mV[VALPHA] *= alpha;
        F32 right = pen;
        font->renderUTF8(body, pos, pen, (F32)y, color, LLFontGL::LEFT, LLFontGL::BOTTOM, style, shadow, run_end - pos, S32_MAX, &right);
        pen = right;
        pos = run_end;
    }
}

void SKFloaterLocalChat::draw()
{
    if (!modernEnabled())
    {
        closeFloater();
        return;
    }

    // In mouselook the chat must not take the mouse from the world.
    const bool opaque = !gAgentCamera.cameraMouselook();
    setMouseOpaque(opaque);
    getDragHandle()->setMouseOpaque(opaque);

    layout();

    static LLCachedControl<S32> style(gSavedSettings, "SkoomaLocalChatStyle", STYLE_CARDS);
    const bool cards = style != STYLE_MINIMAL;
    const bool full = expanded();
    const LLRect content = contentRect();
    const S32 view_height = content.getHeight();
    mScroll = llclamp(mScroll, 0, llmax(0, mContentHeight - view_height));
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (mWasExpanded && !full)
    {
        mCollapsedAt = now;
    }
    mWasExpanded = full;

    if (mCog)
    {
        mCog->setVisible(mHovered && opaque);
    }

    if (opaque && mHovered)
    {
        // The only chrome, the mini-map's: a faint dark tint inside a thin light border.
        static const LLColor4 tint(16.f / 255.f, 16.f / 255.f, 20.f / 255.f, 61.f / 255.f);
        static const LLColor4 border(197.f / 255.f, 205.f / 255.f, 211.f / 255.f, 122.f / 255.f);
        gl_rect_2d(0, getRect().getHeight(), getRect().getWidth(), 0, tint, true);
        gl_rect_2d(0, getRect().getHeight(), getRect().getWidth(), 0, border, false);
    }

    if (!mRows.empty())
    {
        LLLocalClipRect clip(content);
        static LLUIImagePtr card_image = LLUI::getUIImage("Rounded_Square");
        static LLUIColor card_color = LLUIColorTable::instance().getColor("Black");
        const S32 text_x = content.mLeft + (cards ? PAD_X + DISC + DISC_GAP : PAD_X);
        const LLFontGL::ShadowType shadow = cards ? LLFontGL::NO_SHADOW : LLFontGL::DROP_SHADOW_SOFT;
        const S32 base = content.mBottom - mScroll;
        const bool scrolled = mScroll > 0;
        // Scroll masks: what nears the top edge fades out, and the bottom edge too while scrolled up.
        auto edge = [&](S32 lo, S32 hi)
        {
            const F32 mid = 0.5f * (F32)(llmax(lo, content.mBottom) + llmin(hi, content.mTop));
            const F32 top = llclamp(((F32)content.mTop - mid) / EDGE_FADE, 0.f, 1.f);
            return scrolled ? top * llclamp((mid - (F32)content.mBottom) / EDGE_FADE, 0.f, 1.f) : top;
        };

        for (S32 c = (S32)mCards.size() - 1; c >= 0; --c)
        {
            const Card& card = mCards[c];
            const F32 alpha = full ? 1.f : cardAlpha(card, now);
            if (alpha <= 0.f)
            {
                break; // older cards arrived earlier, so they have faded too
            }
            const Row& top_row = mRows[card.mFirstRow];
            const Row& bottom_row = mRows[card.mLastRow];
            const S32 card_bottom = base + bottom_row.mBottom + bottom_row.mGap;
            const S32 card_top = base + top_row.mBottom + top_row.height() + lineHeight(top_row);
            if (card_top < content.mBottom)
            {
                continue;
            }
            if (card_bottom > content.mTop)
            {
                break;
            }

            const Message& first = mMessages[card.mFirstMessage];
            if (cards)
            {
                card_image->draw(content.mLeft, card_bottom, card.mWidth,card_top - card_bottom, card_color.get() % (CARD_ALPHA * alpha * edge(card_bottom, card_top)));
                if (!first.mName.empty())
                {
                    // Round for people, square for objects.
                    gGL.getTextureSlot(0)->unbind();
                    const S32 disc_top = card_top - PAD_Y - 1;
                    LLColor4 disc = first.mNameColor;
                    disc.mV[VALPHA] *= alpha * edge(disc_top - DISC, disc_top);
                    if (first.mSource == CHAT_SOURCE_OBJECT)
                    {
                        gl_rect_2d(content.mLeft + PAD_X, disc_top, content.mLeft + PAD_X + DISC, disc_top - DISC, disc);
                    }
                    else
                    {
                        gGL.color4fv(disc.mV);
                        gl_circle_2d((F32)(content.mLeft + PAD_X) + DISC * 0.5f, (F32)disc_top - DISC * 0.5f, DISC * 0.5f, 16, true);
                    }
                }
            }

            for (S32 r = card.mFirstRow; r <= card.mLastRow; ++r)
            {
                const Row& row = mRows[r];
                const S32 y = base + row.mBottom + row.mGap + row.mPadBottom;
                if (y > content.mTop || y + lineHeight(row) < content.mBottom)
                {
                    continue;
                }
                const F32 row_alpha = alpha * edge(y, y + lineHeight(row));
                if (row_alpha <= 0.f)
                {
                    continue;
                }
                if (row.mMessage < 0)
                {
                    LLColor4 color = first.mNameColor;
                    color.mV[VALPHA] *= row_alpha;
                    name_font()->renderUTF8(first.mName, 0, (F32)text_x, (F32)y, color, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::BOLD, LLFontGL::NO_SHADOW);
                }
                else
                {
                    drawRowText(row, text_x, y, row_alpha, shadow);
                }
            }
        }
    }

    // Jump to present: once scrolled up by more than a view, or from any distance once something new arrived.
    if (mScroll == 0)
    {
        mUnseen = 0;
    }
    mPillRect = LLRect();
    if (mScroll > view_height || mUnseen > 0)
    {
        if (mPillCount != mUnseen)
        {
            mPillCount = mUnseen;
            if (mUnseen > 0)
            {
                LLStringUtil::format_map_t args;
                args["[COUNT]"] = llformat("%d", mUnseen);
                mPillLabel = getString("new_messages", args);
            }
            else
            {
                mPillLabel = getString("jump_to_present");
            }
        }
        LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        const S32 w = font->getWidth(mPillLabel) + 2 * PAD_X + 4;
        const S32 h = font->getLineHeight() + 2 * PAD_Y;
        const S32 left = (getRect().getWidth() - w) / 2;
        const S32 bottom = content.mBottom + 2;
        mPillRect.setOriginAndSize(left, bottom, w, h);
        static LLUIImagePtr pill_image = LLUI::getUIImage("Rounded_Square");
        static LLUIColor pill_color = LLUIColorTable::instance().getColor("Black");
        static LLUIColor pill_text = LLUIColorTable::instance().getColor("White");
        pill_image->draw(mPillRect, pill_color.get() % 0.8f);
        font->renderUTF8(mPillLabel, 0, (F32)(left + w / 2), (F32)(bottom + PAD_Y), pill_text.get(), LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }

    if (mHovered && opaque && mContentHeight > view_height)
    {
        // A thin scroll thumb on hover, its position read from the bottom.
        constexpr S32 THUMB_W = 3;
        const S32 track = view_height;
        const S32 thumb = llmax(12, track * view_height / mContentHeight);
        const S32 range = mContentHeight - view_height;
        const S32 bottom = content.mBottom + (track - thumb) * mScroll / range;
        static LLUIColor thumb_color = LLUIColorTable::instance().getColor("White");
        const S32 right = getRect().getWidth() - 2;
        gl_rect_2d(right - THUMB_W, bottom + thumb, right, bottom, thumb_color.get() % 0.35f);
    }

    LLFloater::draw();
}

std::string SKFloaterLocalChat::urlAt(S32 x, S32 y) const
{
    if (mLayoutDirty || !contentRect().pointInRect(x, y))
    {
        return std::string();
    }
    const S32 text_x = contentRect().mLeft + (mLayoutStyle != STYLE_MINIMAL ? PAD_X + DISC + DISC_GAP : PAD_X);
    const S32 base = contentRect().mBottom - mScroll;
    const bool full = expanded();
    const F64 now = LLFrameTimer::getTotalSeconds();
    for (const Row& row : mRows)
    {
        const S32 y0 = base + row.mBottom + row.mGap + row.mPadBottom;
        if (y < y0 || y >= y0 + lineHeight(row))
        {
            continue;
        }
        const Card& card = mCards[row.mCard];
        if (!full && cardAlpha(card, now) <= 0.f)
        {
            return std::string();
        }
        if (row.mMessage < 0)
        {
            const Message& first = mMessages[card.mFirstMessage];
            return x < text_x + name_font()->getWidth(first.mName) ? first.mSenderUrl : std::string();
        }
        const Message& msg = mMessages[row.mMessage];
        if (x < text_x)
        {
            return std::string();
        }
        const S32 at = row.mBegin + LLViewerChat::getChatFont()->byteFromPixelOffset(msg.mBody, row.mBegin, (F32)(x - text_x), F32_MAX, row.mLength, false);
        if (at >= row.mBegin + row.mLength)
        {
            return std::string();
        }
        if (at < msg.mNameLen)
        {
            return msg.mSenderUrl;
        }
        for (const Link& link : msg.mLinks)
        {
            if (at >= link.mBegin && at < link.mEnd)
            {
                return link.mUrl;
            }
        }
        return std::string();
    }
    return std::string();
}

bool SKFloaterLocalChat::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // The drag handle lies under everything; any other child takes its own clicks.
    LLView* child = childFromPoint(x, y);
    if (mPillRect.pointInRect(x, y))
    {
        mScroll = 0;
        mUnseen = 0;
        mPillRect = LLRect();
        return true;
    }
    if (!child || child == getDragHandle())
    {
        const std::string url = urlAt(x, y);
        if (!url.empty())
        {
            LLUrlAction::clickAction(url, true);
            return true;
        }
    }
    return LLFloater::handleMouseDown(x, y, mask);
}

bool SKFloaterLocalChat::handleHover(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture() && (mPillRect.pointInRect(x, y) || !urlAt(x, y).empty()))
    {
        getWindow()->setCursor(UI_CURSOR_HAND);
        LLFloater::handleHover(x, y, mask);
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    return LLFloater::handleHover(x, y, mask);
}

bool SKFloaterLocalChat::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    const S32 clicks = delta.mClicks;
    // LLFloater::handleScrollWheel always claims the wheel, so only a child that scrolls may take it first.
    if (childrenHandleScrollWheel(x, y, delta))
    {
        return true;
    }
    // Wheel up reads back through history.
    mScroll -= clicks * LLViewerChat::getChatFont()->getLineHeight() * 3;
    mScroll = llclamp(mScroll, 0, llmax(0, mContentHeight - contentRect().getHeight()));
    return true;
}

void SKFloaterLocalChat::onStyleMenu(const LLSD& param)
{
    gSavedSettings.setS32("SkoomaLocalChatStyle", param.asString() == "minimal" ? STYLE_MINIMAL : STYLE_CARDS);
}

bool SKFloaterLocalChat::onStyleCheck(const LLSD& param) const
{
    static LLCachedControl<S32> style(gSavedSettings, "SkoomaLocalChatStyle", STYLE_CARDS);
    return (param.asString() == "minimal") == (style == STYLE_MINIMAL);
}
