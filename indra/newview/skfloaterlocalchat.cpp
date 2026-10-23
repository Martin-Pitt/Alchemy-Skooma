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
#include "llrendertarget.h"
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
#include "alchattimestamp.h"
#include "llagentcamera.h"
#include "llavatariconctrl.h"
#include "llfloaterimcontainer.h"
#include "llfloaterimnearbychat.h"
#include "lllogchat.h"
#include "llviewerchat.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewertexture.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"

namespace
{
constexpr size_t MAX_MESSAGES = 300;
constexpr S32 MAX_CARD_MESSAGES = 8;
constexpr F64 CARD_JOIN_SECONDS = 60.0;
constexpr S32 PAD_X = 8;
constexpr S32 PAD_Y = 5;
constexpr S32 CARD_PAD_X = 10; // a card's sides
constexpr S32 CARD_PAD_Y = 6;  // above the name line and below the last line
constexpr S32 NAME_GAP = 2;    // between the name line and the text
constexpr S32 CARD_GAP = 4;
constexpr S32 MINIMAL_GAP = 2;
constexpr S32 ICON_PX = 28;        // card icon height; avatars are this wide, objects up to ICON_MAX_ASPECT times
constexpr S32 ICON_MAX_ASPECT = 4;
constexpr S32 DISC_GAP = 6;
constexpr F64 GLYPH_CHECK_SECONDS = 0.5; // how often an object speaker's glyph is looked up
constexpr S32 INPUT_HEIGHT = 24;
constexpr S32 INPUT_GAP = 4;
constexpr S32 INSET = 8; // inside the backdrop's border
constexpr F32 EDGE_FADE = 24.f; // the scroll mask at the top, and at the bottom while scrolled
constexpr F32 CARD_ALPHA = 0.6f;
constexpr S32 TIME_GAP = 10;    // between the name and its timestamp
constexpr F32 TIME_ALPHA = 0.45f;
constexpr S32 EMBED_MAX_WIDTH = 360;
constexpr S32 EMBED_MIN_WIDTH = 120;
constexpr S32 EMBED_PAD = 6;
constexpr S32 EMBED_GAP = 4;    // between the text and its preview
constexpr S32 EMBED_ACCENT = 3; // the bar in the speaker's colour
constexpr S32 EMBED_LINE_GAP = 2;

S32 utf8_lead_len(char c)
{
    const U8 b = (U8)c;
    return b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
}

LLFontGL* name_font()
{
    return LLFontGL::getFontSansSerifSmall();
}

LLFontGL* time_font()
{
    return LLFontGL::getFontSansSerifSmall();
}

// The width a bold run draws at: the bold face's, plus the synthesized bold's second pass.
S32 bold_width(const LLFontGL* font, std::string_view text)
{
    return text.empty() ? 0 : font->faceFor(LLFontGL::BOLD)->getWidth(text) + 1;
}

// Set while the messages draw into the mask's target.
bool sDrawingMask = false;

// Into the mask, alpha has to accumulate as coverage. The fonts reset the blend to BT_ALPHA, which squares a shape's
// alpha in the target, so every shape drawn after text asks for this again.
void shape_blend()
{
    if (sDrawingMask)
    {
        gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA, LLRender::BF_ONE, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
    }
}

LLFontGL* embed_font()
{
    return LLFontGL::getFontSansSerifSmall();
}

S32 embed_height()
{
    return 2 * embed_font()->getLineHeight() + EMBED_LINE_GAP + 2 * EMBED_PAD;
}

S32 embed_thumb()
{
    return embed_height() - 2 * EMBED_PAD;
}

bool unfurl_enabled()
{
    static LLCachedControl<bool> unfurl(gSavedSettings, "SkoomaLocalChatUnfurl", true);
    return unfurl;
}

bool embed_shown(const SKChatEmbedPtr& embed)
{
    return embed && embed->mState == SKChatEmbed::READY && !embed->mTitle.empty() && unfurl_enabled();
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
    if (LLAvatarPropertiesProcessor::instanceExists())
    {
        for (const auto& [id, picture] : mAvatarPictures)
        {
            if (picture.mObserving)
            {
                LLAvatarPropertiesProcessor::getInstance()->removeObserver(id, this);
            }
        }
    }
    mMask.release();
}

// static
bool SKFloaterLocalChat::modernEnabled()
{
    static LLCachedControl<bool> modern(gSavedSettings, "SkoomaLocalChatModern", true);
    return modern;
}

// static
bool SKFloaterLocalChat::shouldTakeChat()
{
    if (!modernEnabled())
    {
        return false;
    }
    // As the chat bar: while Conversations is up, starting chat selects its nearby conversation instead.
    LLFloaterIMContainer* conversations = LLFloaterIMContainer::findInstance();
    return !conversations || !conversations->getVisible() || conversations->isMinimized();
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
    static LLCachedControl<bool> show_seconds(gSavedSettings, "AlchemyIMShowSeconds", false);
    msg.mTime = ALChatTimestamp::format(chat.mTimeStr.empty() ? LLLogChat::timestamp2LogString(0, false, true) : chat.mTimeStr, show_seconds);

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
        // As chat history: a web link's query follows its label, in grey.
        const std::string query = match.getQuery();
        if (!query.empty())
        {
            Link tail = link;
            tail.mBegin = link.mEnd;
            out += query;
            tail.mEnd = (S32)out.size();
            tail.mQuery = true;
            msg.mLinks.push_back(std::move(link));
            msg.mLinks.push_back(std::move(tail));
        }
        else
        {
            msg.mLinks.push_back(std::move(link));
        }
        rest.erase(0, match.getEnd() + 1);
    }
    out += rest;
    msg.mBody = std::move(out);

    // One preview a message, for its first link that has one; nothing is looked up until it is drawn.
    if (unfurl_enabled())
    {
        for (const Link& link : msg.mLinks)
        {
            if (!link.mQuery && (msg.mEmbed = mUnfurl.find(link.mUrl)))
            {
                break;
            }
        }
    }

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
    if (row.mEmbed)
    {
        return embed_height();
    }
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
    // A preview that arrived, or the setting turned, lays the messages out again.
    const U32 unfurl_generation = mUnfurl.generation() * 2 + (unfurl_enabled() ? 1 : 0);
    if (!mLayoutDirty && width == mLayoutWidth && style == mLayoutStyle && unfurl_generation == mUnfurlGeneration)
    {
        return;
    }
    mUnfurlGeneration = unfurl_generation;
    ++mLayoutGeneration;
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
    const F64 now = LLFrameTimer::getTotalSeconds();
    const S32 pad_x = cards ? CARD_PAD_X : PAD_X;
    F32 wrap = (F32)llmax(16, width - 2 * pad_x);
    // How much wider a minimal line's bold name draws than the regular face measures it.
    auto bold_extra = [font](const Message& msg)
    {
        const std::string_view name = std::string_view(msg.mBody).substr(0, msg.mNameLen);
        return llmax(0, bold_width(font, name) - font->getWidth(name));
    };

    for (S32 m = 0; m < (S32)mMessages.size(); ++m)
    {
        Message& msg = mMessages[m];
        msg.mNameWidth = bold_width(name_font(), msg.mName);
        msg.mTimeWidth = msg.mName.empty() || msg.mTime.empty() ? 0 : time_font()->getWidth(msg.mTime) + TIME_GAP;
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
            card.mIconWidth = cards && !msg.mName.empty() ? iconWidth(msg, now) : 0;
            card.mTextX = cards ? pad_x + llmax(card.mIconWidth, ICON_PX) + DISC_GAP : pad_x;
            wrap = (F32)llmax(16, width - card.mTextX - pad_x);
            mCards.push_back(card);
            if (cards && !msg.mName.empty())
            {
                Row header;
                header.mMessage = -1;
                header.mCard = (S32)mCards.size() - 1;
                header.mPadTop = CARD_PAD_Y;
                header.mPadBottom = NAME_GAP;
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
            // A minimal line's timestamp sits in its first row, after the name.
            const F32 row_wrap = pos == 0 && msg.mNameLen > 0 ? llmax(16.f, wrap - (F32)(msg.mTimeWidth + bold_extra(msg))) : wrap;
            S32 count = font->maxDrawableBytes(body.substr(pos), row_wrap, line_end - pos, LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
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
                row.mPadTop = cards ? CARD_PAD_Y : 0;
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
        if (embed_shown(msg.mEmbed))
        {
            const SKChatEmbed& embed = *msg.mEmbed;
            const S32 text_w = llmax(embed_font()->getWidth(embed.mTitle) + 2, embed_font()->getWidth(embed.mDescription));
            const S32 thumb = embed.mImage.notNull() ? embed_thumb() + EMBED_PAD : 0;
            const S32 room = llmax(16, llmin(EMBED_MAX_WIDTH, width - mCards.back().mTextX - pad_x));
            msg.mEmbedWidth = llclamp(EMBED_ACCENT + 2 * EMBED_PAD + text_w + thumb, llmin(EMBED_MIN_WIDTH, room), room);
            Row row;
            row.mMessage = m;
            row.mCard = (S32)mCards.size() - 1;
            row.mEmbed = true;
            row.mPadTop = EMBED_GAP;
            mRows.push_back(row);
        }
        mRows.back().mPadBottom = cards ? CARD_PAD_Y : 0;
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
            if (row.mMessage < 0)
            {
                const Message& first = mMessages[mCards[row.mCard].mFirstMessage];
                widest = llmax(widest, first.mNameWidth + first.mTimeWidth);
            }
            else if (row.mEmbed)
            {
                widest = llmax(widest, mMessages[row.mMessage].mEmbedWidth);
            }
            else
            {
                const Message& msg = mMessages[row.mMessage];
                const S32 lead = row.mBegin == 0 && msg.mNameLen > 0 ? msg.mTimeWidth + bold_extra(msg) : 0;
                widest = llmax(widest, font->getWidthBytes(msg.mBody, row.mBegin, row.mLength) + lead);
            }
        }
        card.mWidth = llmin(width, card.mTextX + widest + pad_x);

        // Tall enough for its icon.
        if (card.mIconWidth > 0)
        {
            S32 height = -mRows[card.mLastRow].mGap;
            for (S32 r = card.mFirstRow; r <= card.mLastRow; ++r)
            {
                height += mRows[r].height() + lineHeight(mRows[r]);
            }
            mRows[card.mLastRow].mPadBottom += llmax(0, ICON_PX + 2 * CARD_PAD_Y + 1 - height);
        }
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

void SKFloaterLocalChat::drawTime(const Message& msg, F32 x, S32 y, F32 alpha, LLFontGL::ShadowType shadow) const
{
    LLColor4 color = msg.mNameColor;
    color.mV[VALPHA] *= TIME_ALPHA * alpha;
    time_font()->renderUTF8(msg.mTime, 0, x, (F32)y, color, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, shadow);
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
                    color = link.mQuery ? LLColor4::grey : link_color.get();
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
        if (pos == msg.mNameLen && row.mBegin == 0 && msg.mTimeWidth > 0)
        {
            drawTime(msg, pen + (F32)TIME_GAP, y, alpha, shadow);
            pen += (F32)msg.mTimeWidth;
        }
    }
}

void SKFloaterLocalChat::requestEmbeds(const Card& card)
{
    if (!unfurl_enabled())
    {
        return;
    }
    for (S32 m = card.mFirstMessage; m <= card.mLastMessage; ++m)
    {
        const SKChatEmbedPtr& embed = mMessages[m].mEmbed;
        if (embed && embed->mState == SKChatEmbed::IDLE)
        {
            mUnfurl.request(embed);
        }
    }
}

void SKFloaterLocalChat::drawEmbed(const Message& msg, S32 x, S32 y, F32 alpha) const
{
    static LLUIImagePtr card_image = LLUI::getUIImage("Rounded_Square");
    static LLUIColor card_color = LLUIColorTable::instance().getColor("Black");
    static LLUIColor title_color = LLUIColorTable::instance().getColor("White");
    const SKChatEmbed& embed = *msg.mEmbed;
    const S32 w = msg.mEmbedWidth;
    const S32 h = embed_height();
    shape_blend();
    card_image->draw(x, y, w, h, card_color.get() % (0.45f * alpha));

    gGL.getTextureSlot(0)->unbind();
    LLColor4 accent = msg.mNameColor;
    accent.mV[VALPHA] *= alpha;
    gl_rect_2d(x + 1, y + h - 3, x + 1 + EMBED_ACCENT, y + 3, accent);

    S32 right = x + w - EMBED_PAD;
    if (embed.mImage.notNull())
    {
        const S32 thumb = embed_thumb();
        right -= thumb;
        embed.mImage->addTextureStats((F32)(thumb * thumb));
        if (embed.mImage->hasGLTexture())
        {
            gl_draw_scaled_image(right, y + EMBED_PAD, thumb, thumb, embed.mImage, LLColor4(1.f, 1.f, 1.f, alpha));
        }
        right -= EMBED_PAD;
    }

    LLFontGL* font = embed_font();
    const S32 left = x + EMBED_ACCENT + EMBED_PAD;
    const S32 max_px = llmax(0, right - left);
    const S32 line = font->getLineHeight();
    LLColor4 title = title_color.get();
    title.mV[VALPHA] *= alpha;
    font->renderUTF8(embed.mTitle, 0, (F32)left, (F32)(y + EMBED_PAD + line + EMBED_LINE_GAP), title, LLFontGL::LEFT, LLFontGL::BOTTOM,
                     LLFontGL::BOLD, LLFontGL::NO_SHADOW, S32_MAX, max_px, nullptr, true);
    if (!embed.mDescription.empty())
    {
        LLColor4 desc = title;
        desc.mV[VALPHA] *= 0.7f;
        font->renderUTF8(embed.mDescription, 0, (F32)left, (F32)(y + EMBED_PAD), desc, LLFontGL::LEFT, LLFontGL::BOTTOM,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, max_px, nullptr, true);
    }
}

void SKFloaterLocalChat::drawMessages(const LLRect& content, F64 now, bool full, bool cards)
{
    static LLUIImagePtr card_image = LLUI::getUIImage("Rounded_Square");
    static LLUIColor card_color = LLUIColorTable::instance().getColor("Black");
    const LLFontGL::ShadowType shadow = cards ? LLFontGL::NO_SHADOW : LLFontGL::DROP_SHADOW_SOFT;
    const S32 base = content.mBottom - mScroll;
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
        const S32 text_x = content.mLeft + card.mTextX;
        if (cards)
        {
            shape_blend();
            card_image->draw(content.mLeft, card_bottom, card.mWidth, card_top - card_bottom, card_color.get() % (CARD_ALPHA * alpha));
            if (!first.mName.empty())
            {
                // Round for people, square for objects.
                gGL.getTextureSlot(0)->unbind();
                const S32 disc_top = card_top - CARD_PAD_Y - 1;
                LLColor4 disc = first.mNameColor;
                disc.mV[VALPHA] *= alpha;
                if (first.mSource == CHAT_SOURCE_OBJECT)
                {
                    drawObjectIcon(first, content.mLeft + CARD_PAD_X, disc_top, card.mIconWidth, disc, now);
                }
                else
                {
                    drawAvatarIcon(first, content.mLeft + CARD_PAD_X, disc_top, disc);
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
            if (row.mMessage < 0)
            {
                LLColor4 color = first.mNameColor;
                color.mV[VALPHA] *= alpha;
                name_font()->renderUTF8(first.mName, 0, (F32)text_x, (F32)y, color, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::BOLD, LLFontGL::NO_SHADOW);
                if (first.mTimeWidth > 0)
                {
                    drawTime(first, (F32)(text_x + first.mNameWidth + TIME_GAP), y, alpha, LLFontGL::NO_SHADOW);
                }
            }
            else if (row.mEmbed)
            {
                drawEmbed(mMessages[row.mMessage], text_x, y, alpha);
            }
            else
            {
                drawRowText(row, text_x, y, alpha, shadow);
            }
        }
    }
}

void SKFloaterLocalChat::drawMaskedMessages(const LLRect& content, F64 now, bool full, bool cards)
{
    // What the frame shows: the visible cards' fades, and whether any of them reaches into an edge band.
    const S32 base = content.mBottom - mScroll;
    const bool scrolled = mScroll > 0;
    const S32 top_band = content.mTop - (S32)EDGE_FADE;
    const S32 bottom_band = content.mBottom + (S32)EDGE_FADE;
    bool in_band = false;
    U64 fades = 1469598103934665603ULL;
    U64 icons = 1469598103934665603ULL; // the icons as last drawn, so a picture or glyph arriving redraws the mask
    for (S32 c = (S32)mCards.size() - 1; c >= 0; --c)
    {
        const Card& card = mCards[c];
        const F32 alpha = full ? 1.f : cardAlpha(card, now);
        if (alpha <= 0.f)
        {
            break;
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
        in_band |= card_top > top_band || (scrolled && card_bottom < bottom_band);
        fades = (fades ^ (U64)(c * 256 + ll_round(alpha * 255.f))) * 1099511628211ULL;
        icons = (icons ^ iconStamp(mMessages[card.mFirstMessage])) * 1099511628211ULL;
        requestEmbeds(card);
        for (S32 m = card.mFirstMessage; m <= card.mLastMessage; ++m)
        {
            // A preview's picture arriving changes what the mask holds.
            const SKChatEmbedPtr& embed = mMessages[m].mEmbed;
            if (embed && embed->mImage.notNull())
            {
                fades = (fades ^ (U64)(embed->mImage->hasGLTexture() ? 64 + embed->mImage->getDiscardLevel() : 63)) * 1099511628211ULL;
            }
        }
    }

    if (!in_band)
    {
        LLLocalClipRect clip(content);
        drawMessages(content, now, full, cards);
        return;
    }

    // A mask as CSS draws one: the messages go to a target of their own, laid over the world through a gradient.
    const LLVector3 scale = gGL.getUIScale();
    const U32 width = (U32)llmax(1, ll_round(content.getWidth() * scale.mV[VX]));
    const U32 height = (U32)llmax(1, ll_round(content.getHeight() * scale.mV[VY]));
    const MaskKey key{ width, height, mScroll, mLayoutGeneration, fades, icons, cards };
    if (mMask.getWidth() != width || mMask.getHeight() != height)
    {
        mMask.release();
        mMaskKey = MaskKey();
        if (!mMask.allocate(width, height, GL_RGBA8))
        {
            mMask.release();
            LLLocalClipRect clip(content);
            drawMessages(content, now, full, cards);
            return;
        }
    }
    if (!(key == mMaskKey))
    {
        mMaskKey = key;
        LLGLDisable scissor(GL_SCISSOR_TEST);
        mMask.bindTarget();
        const LLColor4 clear_color = gGL.getClearColor();
        gGL.setClearColor(LLColor4::transparent);
        mMask.clear(GL_COLOR_BUFFER_BIT);
        gGL.setClearColor(clear_color);
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.ortho(0.f, (F32)width, 0.f, (F32)height, -1.f, 1.f);
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();
        LLUI::pushMatrix();
        LLUI::loadIdentity();
        gGL.scaleUI(scale.mV[VX], scale.mV[VY], 1.f);
        LLUI::translate((F32)-content.mLeft, (F32)-content.mBottom);
        // Premultiplied: colour blends over, alpha accumulates as coverage.
        sDrawingMask = true;
        shape_blend();
        drawMessages(content, now, full, cards);
        gGL.flush();
        sDrawingMask = false;
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
        LLUI::popMatrix();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
        mMask.flush();
    }

    // Three bands: fading in under the top edge, whole, and fading out over the bottom one while scrolled.
    const F32 l = (F32)content.mLeft;
    const F32 r = (F32)content.mRight;
    const F32 b = (F32)content.mBottom;
    const F32 t = (F32)content.mTop;
    const F32 fade = llmin(EDGE_FADE, 0.5f * (t - b));
    const F32 ys[4] = { b, scrolled ? b + fade : b, t - fade, t };
    const F32 as[4] = { scrolled ? 0.f : 1.f, 1.f, 1.f, 0.f };
    LLLocalClipRect clip(content);
    gGL.getTextureSlot(0)->bind(&mMask);
    gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
    gGL.begin(LLRender::TRIANGLES);
    for (S32 i = 0; i < 3; ++i)
    {
        const F32 y0 = ys[i], y1 = ys[i + 1];
        if (y1 <= y0)
        {
            continue;
        }
        const F32 v0 = (y0 - b) / (t - b), v1 = (y1 - b) / (t - b);
        const F32 a0 = as[i], a1 = as[i + 1];
        gGL.color4f(a0, a0, a0, a0); gGL.texCoord2f(0.f, v0); gGL.vertex2f(l, y0);
        gGL.color4f(a0, a0, a0, a0); gGL.texCoord2f(1.f, v0); gGL.vertex2f(r, y0);
        gGL.color4f(a1, a1, a1, a1); gGL.texCoord2f(1.f, v1); gGL.vertex2f(r, y1);
        gGL.color4f(a0, a0, a0, a0); gGL.texCoord2f(0.f, v0); gGL.vertex2f(l, y0);
        gGL.color4f(a1, a1, a1, a1); gGL.texCoord2f(1.f, v1); gGL.vertex2f(r, y1);
        gGL.color4f(a1, a1, a1, a1); gGL.texCoord2f(0.f, v1); gGL.vertex2f(l, y1);
    }
    gGL.end();
    gGL.flush();
    gGL.getTextureSlot(0)->unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
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

    mUnfurl.update();
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
        drawMaskedMessages(content, now, full, cards);
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
        const S32 text_x = contentRect().mLeft + card.mTextX;
        if (!full && cardAlpha(card, now) <= 0.f)
        {
            return std::string();
        }
        if (row.mMessage < 0)
        {
            const Message& first = mMessages[card.mFirstMessage];
            return x < text_x + first.mNameWidth ? first.mSenderUrl : std::string();
        }
        if (row.mEmbed)
        {
            const Message& msg = mMessages[row.mMessage];
            return msg.mEmbed && x >= text_x && x < text_x + msg.mEmbedWidth ? msg.mEmbed->mUrl : std::string();
        }
        const Message& msg = mMessages[row.mMessage];
        if (x < text_x)
        {
            return std::string();
        }
        F32 offset = (F32)(x - text_x);
        if (row.mBegin == 0 && msg.mNameLen > 0 && msg.mTimeWidth > 0)
        {
            // Past the name, the timestamp pushes the rest of the row along.
            const F32 name_end = (F32)LLViewerChat::getChatFont()->getWidthBytes(msg.mBody, 0, msg.mNameLen);
            if (offset >= name_end)
            {
                offset = offset < name_end + msg.mTimeWidth ? -1.f : offset - msg.mTimeWidth;
            }
        }
        if (offset < 0.f)
        {
            return std::string();
        }
        const S32 at = row.mBegin + LLViewerChat::getChatFont()->byteFromPixelOffset(msg.mBody, row.mBegin, offset, F32_MAX, row.mLength, false);
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

namespace
{
// Whether an avatar sits on the root or one of its prims, as the outline capture finds a vehicle's crew.
bool hasSeatedAvatar(const LLViewerObject* root)
{
    for (const LLViewerObject* child : root->getChildren())
    {
        if (!child)
        {
            continue;
        }
        if (child->isAvatar())
        {
            return true;
        }
        for (const LLViewerObject* sitter : child->getChildren())
        {
            if (sitter && sitter->isAvatar())
            {
                return true;
            }
        }
    }
    return false;
}
} // namespace

const SKOutlineLayer* SKFloaterLocalChat::objectGlyph(const LLUUID& speaker, F64 now)
{
    if (speaker.isNull())
    {
        return nullptr;
    }
    if (mObjectGlyphs.size() > MAX_MESSAGES)
    {
        mObjectGlyphs.clear(); // more speakers than the log holds; the shown ones are looked up again
    }
    ObjectGlyph& g = mObjectGlyphs[speaker];
    // Asking keeps the capture alive while its cards are drawn; once they fade it expires on its own.
    if (now - g.mChecked > GLYPH_CHECK_SECONDS)
    {
        g.mChecked = now;
        g.mShown = false;
        LLViewerObject* obj = gObjectList.findObject(speaker);
        LLViewerObject* root = obj && !obj->isDead() ? obj->getRootEdit() : nullptr;
        if (root && !root->isAvatar())
        {
            // A vehicle (someone seated on it) reads best from above, anything else from the side.
            const SKOutlinePolicy::View view = hasSeatedAvatar(root) ? SKOutlinePolicy::View::PLAN : SKOutlinePolicy::View::SIDE;
            const SKOutlineGlyph* glyph = SKOutlineCapture::glyphFor(root->getID(), view);
            if (glyph && !glyph->layers.empty() && glyph->layers[0].texture)
            {
                g.mLayer = glyph->layers[0];
                g.mShown = true;
            }
        }
    }
    return g.mShown ? &g.mLayer : nullptr;
}

S32 SKFloaterLocalChat::iconWidth(const Message& first, F64 now)
{
    if (first.mSource != CHAT_SOURCE_OBJECT)
    {
        return ICON_PX;
    }
    S32 width = ICON_PX;
    if (const SKOutlineLayer* layer = objectGlyph(first.mFromID, now))
    {
        const F32 w = layer->x1 - layer->x0;
        const F32 h = layer->y1 - layer->y0;
        if (w > 0.f && h > 0.f)
        {
            width = llclamp((S32)std::lround((F32)ICON_PX * w / h), ICON_PX, ICON_PX * ICON_MAX_ASPECT);
        }
    }
    return width;
}

void SKFloaterLocalChat::drawObjectIcon(const Message& first, S32 left, S32 top, S32 width, const LLColor4& color, F64 now)
{
    // A glyph that came, went or changed shape since the card was laid out: lay out again next frame.
    if (iconWidth(first, now) != width)
    {
        invalidateLayout();
    }
    const SKOutlineLayer* layer = objectGlyph(first.mFromID, now);
    const F32 w = layer ? layer->x1 - layer->x0 : 0.f;
    const F32 h = layer ? layer->y1 - layer->y0 : 0.f;
    if (w <= 0.f || h <= 0.f)
    {
        gl_rect_2d(left, top, left + ICON_PX, top - ICON_PX, color);
        return;
    }

    // The silhouette alone in the speaker's colour, fitted to the card's icon size with its aspect kept.
    const S32 box = llmax(width, ICON_PX);
    const F32 scale = llmin((F32)box / w, (F32)ICON_PX / h);
    const F32 gw = w * scale;
    const F32 gh = h * scale;
    const F32 x0 = (F32)left + ((F32)box - gw) * 0.5f;
    const F32 y0 = (F32)(top - ICON_PX) + ((F32)ICON_PX - gh) * 0.5f;
    gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, layer->texture);
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::TRIANGLE_STRIP);
    gGL.texCoord2f(layer->u0, layer->v0);
    gGL.vertex2f(x0, y0);
    gGL.texCoord2f(layer->u1, layer->v0);
    gGL.vertex2f(x0 + gw, y0);
    gGL.texCoord2f(layer->u0, layer->v1);
    gGL.vertex2f(x0, y0 + gh);
    gGL.texCoord2f(layer->u1, layer->v1);
    gGL.vertex2f(x0 + gw, y0 + gh);
    gGL.end();
    gGL.getTextureSlot(0)->unbind();
}

U64 SKFloaterLocalChat::iconStamp(const Message& first) const
{
    U64 stamp = 0;
    if (first.mSource == CHAT_SOURCE_OBJECT)
    {
        const auto it = mObjectGlyphs.find(first.mFromID);
        if (it != mObjectGlyphs.end() && it->second.mShown)
        {
            const SKOutlineLayer& layer = it->second.mLayer;
            stamp = (U64)layer.texture << 32 ^ (U64)std::hash<F32>()(layer.u0) ^ (U64)std::hash<F32>()(layer.v0) << 7 ^
                    (U64)std::hash<F32>()(layer.u1) << 13 ^ (U64)std::hash<F32>()(layer.v1) << 19 ^ 1;
        }
    }
    else
    {
        const auto it = mAvatarPictures.find(first.mFromID);
        if (it != mAvatarPictures.end() && it->second.mTexture && it->second.mTexture->hasGLTexture())
        {
            stamp = (U64)(uintptr_t)it->second.mTexture.get() ^ (U64)(it->second.mTexture->getDiscardLevel() + 2) << 56;
        }
    }
    return stamp;
}

void SKFloaterLocalChat::drawAvatarIcon(const Message& first, S32 left, S32 top, const LLColor4& color)
{
    const F32 cx = (F32)left + ICON_PX * 0.5f;
    const F32 cy = (F32)top - ICON_PX * 0.5f;
    const F32 radius = ICON_PX * 0.5f;
    LLViewerFetchedTexture* tex = nullptr;
    if (first.mSource == CHAT_SOURCE_AGENT && first.mFromID.notNull())
    {
        AvatarPicture& picture = mAvatarPictures[first.mFromID];
        if (!picture.mAsked)
        {
            // As LLAvatarIconCtrl does: the shared icon id cache first, a legacy properties request otherwise.
            picture.mAsked = true;
            const LLUUID* icon = LLAvatarIconIDCache::getInstance()->get(first.mFromID);
            if (icon && icon->notNull())
            {
                picture.mTexture = LLViewerTextureManager::getFetchedTexture(*icon, FTT_DEFAULT, true, LLGLTexture::BOOST_ICON);
            }
            else if (!icon)
            {
                LLAvatarPropertiesProcessor* app = LLAvatarPropertiesProcessor::getInstance();
                app->addObserver(first.mFromID, this);
                app->sendAvatarLegacyPropertiesRequest(first.mFromID);
                picture.mObserving = true;
            }
        }
        tex = picture.mTexture.get();
    }
    if (tex)
    {
        tex->addTextureStats((F32)(ICON_PX * ICON_PX * 4));
    }
    if (!tex || !tex->hasGLTexture())
    {
        gGL.color4fv(color.mV);
        gl_circle_2d(cx, cy, radius, 16, true);
        return;
    }

    // The picture clipped to a circle, inside a ring of the speaker's colour.
    gGL.getTextureSlot(0)->bindSampled(tex, ALSamplers::BilinearClamp);
    gGL.color4f(1.f, 1.f, 1.f, color.mV[VALPHA]);
    constexpr S32 STEPS = 20;
    const F32 inner = radius - 1.f;
    gGL.begin(LLRender::TRIANGLE_FAN);
    gGL.texCoord2f(0.5f, 0.5f);
    gGL.vertex2f(cx, cy);
    for (S32 i = 0; i <= STEPS; ++i)
    {
        const F32 a = F_TWO_PI * (F32)i / (F32)STEPS;
        const F32 c = cosf(a);
        const F32 sn = sinf(a);
        gGL.texCoord2f(0.5f + 0.5f * c, 0.5f + 0.5f * sn);
        gGL.vertex2f(cx + inner * c, cy + inner * sn);
    }
    gGL.end();
    gGL.getTextureSlot(0)->unbind();
    gGL.color4fv(color.mV);
    gl_circle_2d(cx, cy, radius - 0.5f, STEPS, false);
}

void SKFloaterLocalChat::processProperties(void* data, EAvatarProcessorType type)
{
    LLUUID avatar_id, image_id;
    if (type == APT_PROPERTIES_LEGACY && data)
    {
        const LLAvatarLegacyData* avatar = static_cast<const LLAvatarLegacyData*>(data);
        avatar_id = avatar->avatar_id;
        image_id = avatar->image_id;
    }
    else if (type == APT_PROPERTIES && data)
    {
        const LLAvatarData* avatar = static_cast<const LLAvatarData*>(data);
        avatar_id = avatar->avatar_id;
        image_id = avatar->image_id;
    }
    const auto it = mAvatarPictures.find(avatar_id);
    if (avatar_id.isNull() || it == mAvatarPictures.end())
    {
        return;
    }
    LLAvatarIconIDCache::getInstance()->add(avatar_id, image_id);
    LLAvatarPropertiesProcessor::getInstance()->removeObserver(avatar_id, this);
    it->second.mObserving = false;
    if (image_id.notNull())
    {
        it->second.mTexture = LLViewerTextureManager::getFetchedTexture(image_id, FTT_DEFAULT, true, LLGLTexture::BOOST_ICON);
    }
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
