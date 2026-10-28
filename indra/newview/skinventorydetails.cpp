/**
 * @file skinventorydetails.cpp
 * @brief Inventory Explorer's details: the rows about one item, and the players that preview it
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "skinventorydetails.h"

#include "llagent.h"
#include "llaudioengine.h"
#include "llavatarnamecache.h"
#include "llexperiencecache.h"
#include "llfetchedgltfmaterial.h"
#include "llfontgl.h"
#include "llgltfmateriallist.h"
#include "llgltfmaterialpreviewmgr.h"
#include "llhandmotion.h"
#include "llinventorymodel.h"
#include "lllandmark.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "llscrollbar.h"
#include "lltooltip.h"
#include "lltrans.h"
#include "llui.h"
#include "lluiimage.h"
#include "llurlaction.h"
#include "lluri.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewermessage.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llwindow.h"
#include "llworldmap.h"
#include "llworldmapmessage.h"
#include "lldbstrings.h"
#include "skinventoryexplorerassets.h"
#include "skinventoryexplorerpopup.h"

#include <numeric>

static LLDefaultChildRegistry::Register<SKInventoryDetailsView> r_sk_inventory_details("sk_inventory_details");

namespace
{
    constexpr S32 PAD = 8;
    constexpr S32 ROW_GAP = 3;
    constexpr S32 LABEL_WIDTH = 84;
    constexpr S32 MEDIA_HEIGHT = 150;       // the thumbnail area
    constexpr S32 WAVE_HEIGHT = 90;
    constexpr S32 TIMELINE_HEIGHT = 70;
    constexpr S32 MAP_HEIGHT = 190;
    constexpr S32 BUTTON_HEIGHT = 22;
    constexpr S32 BUTTON_GAP = 4;
    constexpr S32 BAR_WIDTH = 2;
    constexpr S32 BAR_GAP = 1;
    constexpr F32 SETTLE_SECONDS = 0.25f;
    constexpr F32 ZOOM_EASE = 0.08f;
    constexpr F32 ZOOM_MAX = 8.f;
    constexpr S32 WHEEL_PIXELS = 30;

    std::map<std::string, std::string> sStrings;
    SKInventoryListLabels sLabels;
    LLHandle<LLView> sAspectMenu;

    const LLColor4 ACCENT(0.f, 161.f / 255.f, 223.f / 255.f, 1.f);

    LLColor4 hex(U32 rgb, F32 alpha = 1.f)
    {
        return LLColor4(((rgb >> 16) & 0xff) / 255.f, ((rgb >> 8) & 0xff) / 255.f, (rgb & 0xff) / 255.f, alpha);
    }

    // §13.3: bass, mids and treble shades for idle, hover and played bars.
    const U32 SHADES[3][3] = {
        { 0x5d636c, 0x7c838e, 0xa7adb6 },
        { 0x878d97, 0xb0b6c0, 0xd6dae0 },
        { 0x006c96, 0x00a1df, 0x62c9f0 },
    };

    constexpr size_t LONGEST_TAG = 7;   // " (16:9)"

    // The description with any aspect tail taken off.
    std::string without_aspect(const std::string& desc)
    {
        S32 w = 1;
        S32 h = 1;
        std::string base = desc;
        if (SKInventoryDetails::aspectOf(desc, w, h))
        {
            base.erase(base.rfind('('));
            LLStringUtil::trimTail(base);
        }
        return base;
    }

    const S32 ASPECTS[][2] = { { 1, 1 }, { 4, 3 }, { 3, 2 }, { 16, 9 }, { 2, 1 }, { 3, 4 }, { 2, 3 }, { 9, 16 }, { 1, 2 } };

    // A link resolved to what it points at.
    LLViewerInventoryItem* target_item(const LLUUID& id)
    {
        LLViewerInventoryItem* item = gInventory.getItem(id);
        if (item && item->getIsLinkType())
        {
            if (LLViewerInventoryItem* linked = item->getLinkedItem())
            {
                return linked;
            }
        }
        return item;
    }

    std::string format_args(const std::string& text, const LLStringUtil::format_map_t& args)
    {
        std::string out = text;
        LLStringUtil::format(out, args);
        return out;
    }

    std::string seconds(F32 value)
    {
        return llformat("%.1fs", value);
    }

    // "Parcel, Region (x, y, z) - Rating", as new landmarks describe themselves.
    bool parse_location(const std::string& text, std::string& parcel, std::string& region, S32& x, S32& y, S32& z, std::string& rating)
    {
        const size_t open = text.rfind('(');
        const size_t close = text.find(')', open == std::string::npos ? 0 : open);
        if (open == std::string::npos || close == std::string::npos || open == 0)
        {
            return false;
        }
        if (sscanf(text.substr(open + 1, close - open - 1).c_str(), "%d, %d, %d", &x, &y, &z) != 3
            && sscanf(text.substr(open + 1, close - open - 1).c_str(), "%d,%d,%d", &x, &y, &z) != 3)
        {
            return false;
        }
        std::string head = text.substr(0, open);
        LLStringUtil::trim(head);
        const size_t comma = head.rfind(", ");
        region = comma == std::string::npos ? head : head.substr(comma + 2);
        parcel = comma == std::string::npos ? std::string() : head.substr(0, comma);
        rating.clear();
        const size_t dash = text.find(" - ", close);
        if (dash != std::string::npos)
        {
            rating = text.substr(dash + 3);
            LLStringUtil::trim(rating);
        }
        return !region.empty();
    }

    // Drawn with the sampler the caller names: the texture keeps none of its own (altextureslot.h).
    void draw_image(const LLRect& rect, LLTexture* texture, const LLRectf& uv, ALSampler sampler)
    {
        gGL.getTextureSlot(0)->bindSampled(texture, sampler);
        gGL.color4fv(LLColor4::white.mV);
        gGL.begin(LLRender::TRIANGLES);
        gGL.texCoord2f(uv.mRight, uv.mTop);
        gGL.vertex2i(rect.mRight, rect.mTop);
        gGL.texCoord2f(uv.mLeft, uv.mTop);
        gGL.vertex2i(rect.mLeft, rect.mTop);
        gGL.texCoord2f(uv.mLeft, uv.mBottom);
        gGL.vertex2i(rect.mLeft, rect.mBottom);
        gGL.texCoord2f(uv.mRight, uv.mTop);
        gGL.vertex2i(rect.mRight, rect.mTop);
        gGL.texCoord2f(uv.mLeft, uv.mBottom);
        gGL.vertex2i(rect.mLeft, rect.mBottom);
        gGL.texCoord2f(uv.mRight, uv.mBottom);
        gGL.vertex2i(rect.mRight, rect.mBottom);
        gGL.end();
        gGL.getTextureSlot(0)->unbind();
    }

    // The types the player previews: textures and photos, sounds, animations, materials and landmarks.
    bool has_asset_preview(LLAssetType::EType type)
    {
        switch (type)
        {
            case LLAssetType::AT_TEXTURE:
            case LLAssetType::AT_SOUND:
            case LLAssetType::AT_ANIMATION:
            case LLAssetType::AT_MATERIAL:
            case LLAssetType::AT_LANDMARK:
                return true;
            default:
                return false;
        }
    }

    // The texture is asked for the pixels it covers, HiDPI included, with no boost.
    void report_drawn(LLViewerFetchedTexture* texture, S32 width, S32 height)
    {
        const F32 scale = LLUI::getScaleFactor().mV[VX];
        const S32 w = llceil(width * scale);
        const S32 h = llceil(height * scale);
        texture->setKnownDrawSize(w, h);
        texture->addTextureStats((F32)(w * h));
    }

    void draw_chip(S32 x, S32 y, const std::string& text, const LLColor4& color = LLColor4::white)
    {
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        const S32 width = font->getWidth(text) + 8;
        gl_rect_2d(x, y, x + width, y - 14, LLColor4(0.f, 0.f, 0.f, 0.65f), true);
        font->renderUTF8(text, 0, (S32)(x + 4), (S32)(y - 7), color, LLFontGL::LEFT, LLFontGL::VCENTER);
    }
}

namespace SKInventoryDetails
{
    void setStrings(const std::map<std::string, std::string>& strings, const SKInventoryListLabels& labels)
    {
        sStrings = strings;
        sLabels = labels;
    }

    const std::string& str(const std::string& key)
    {
        auto it = sStrings.find(key);
        return it != sStrings.end() ? it->second : LLStringUtil::null;
    }

    const SKInventoryListLabels& labels()
    {
        return sLabels;
    }

    bool aspectOf(const std::string& desc, S32& w, S32& h)
    {
        w = 1;
        h = 1;
        const size_t open = desc.rfind('(');
        if (open == std::string::npos || desc.empty() || desc.back() != ')')
        {
            return false;
        }
        const std::string inner = desc.substr(open + 1, desc.size() - open - 2);
        const size_t colon = inner.find(':');
        if (colon == std::string::npos || colon < 1 || colon > 2 || inner.size() - colon - 1 < 1 || inner.size() - colon - 1 > 2)
        {
            return false;
        }
        if (!std::all_of(inner.begin(), inner.end(), [](char c) { return c == ':' || (c >= '0' && c <= '9'); }))
        {
            return false;
        }
        const S32 pw = atoi(inner.substr(0, colon).c_str());
        const S32 ph = atoi(inner.substr(colon + 1).c_str());
        if (pw <= 0 || ph <= 0)
        {
            return false;
        }
        w = pw;
        h = ph;
        return true;
    }

    bool naturalAspect(const LLUUID& image_id, S32& w, S32& h)
    {
        w = 1;
        h = 1;
        // Only a texture already known is looked at; this asks for no fetch.
        LLViewerFetchedTexture* texture = image_id.notNull() ? LLViewerTextureManager::findFetchedTexture(image_id, TEX_LIST_STANDARD) : nullptr;
        if (!texture || texture->getFullWidth() <= 0 || texture->getFullHeight() <= 0)
        {
            return false;
        }
        const S32 divisor = std::gcd(texture->getFullWidth(), texture->getFullHeight());
        w = texture->getFullWidth() / divisor;
        h = texture->getFullHeight() / divisor;
        return true;
    }

    void aspectFor(const LLInventoryItem* item, S32& w, S32& h)
    {
        w = 1;
        h = 1;
        if (!item || aspectOf(item->getDescription(), w, h) || item->getType() != LLAssetType::AT_TEXTURE)
        {
            return;
        }
        naturalAspect(item->getAssetUUID(), w, h);
    }

    bool canSetAspect(const LLUUID& item_id, std::string* why)
    {
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item || item->getIsLinkType())
        {
            return false;
        }
        if (gInventory.isObjectDescendentOf(item_id, gInventory.getLibraryRootFolderID()))
        {
            if (why)
            {
                *why = str("aspect_library");
            }
            return false;
        }
        if (!item->getPermissions().allowModifyBy(gAgentID))
        {
            if (why)
            {
                *why = str("aspect_no_modify");
            }
            return false;
        }
        // The user's text is never rewritten: refused rather than trimmed or cleaned.
        const std::string base = without_aspect(item->getDescription());
        if (std::any_of(base.begin(), base.end(), [](char c) { return c < 0x20 || c >= 0x7f || c == '|'; }))
        {
            if (why)
            {
                *why = str("aspect_bad_chars");
            }
            return false;
        }
        if (base.size() + LONGEST_TAG > (size_t)DB_INV_ITEM_DESC_STR_LEN)
        {
            if (why)
            {
                *why = str("aspect_too_long");
            }
            return false;
        }
        return true;
    }

    void setAspect(const LLUUID& item_id, S32 w, S32 h)
    {
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item || !canSetAspect(item_id))
        {
            return;
        }
        // Only the tail changes: the old one goes, and the texture's own width:height (1:1 until known) leaves none.
        S32 nw = 1;
        S32 nh = 1;
        naturalAspect(item->getAssetUUID(), nw, nh);
        const std::string base = without_aspect(item->getDescription());
        const std::string tail = (w * nh == h * nw) ? std::string() : llformat("(%d:%d)", w, h);
        const std::string result = tail.empty() ? base : (base.empty() ? tail : base + " " + tail);
        if (result == item->getDescription())
        {
            return;
        }
        LLPointer<LLViewerInventoryItem> new_item = new LLViewerInventoryItem(item);
        new_item->setDescription(result);
        update_inventory_item(new_item, nullptr);
        gInventory.updateItem(new_item);
        gInventory.notifyObservers();
    }

    void hideAspectMenu()
    {
        if (LLView* menu = sAspectMenu.get())
        {
            static_cast<SKExplorerPopupList*>(menu)->hide();
        }
    }

    void showAspectMenu(LLView* spawn, const LLRect& rect, const LLUUID& item_id, bool above)
    {
        SKExplorerPopupList* menu = static_cast<SKExplorerPopupList*>(sAspectMenu.get());
        if (menu && menu->isOpen())
        {
            menu->hide();
            return;
        }
        if (!menu)
        {
            menu = SKExplorerPopupList::create();
            sAspectMenu = menu->getHandle();
        }
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        S32 cur_w = 1;
        S32 cur_h = 1;
        aspectFor(item, cur_w, cur_h);
        std::vector<SKPopupRow> rows;
        for (const auto& aspect : ASPECTS)
        {
            SKPopupRow row;
            const bool current = aspect[0] * cur_h == aspect[1] * cur_w;
            row.mLabel = (current ? "\xE2\x9C\x93 " : "    ") + llformat("%d:%d", aspect[0], aspect[1]);
            row.mValue = LLSD().with("w", aspect[0]).with("h", aspect[1]);
            rows.push_back(row);
        }
        menu->setRows(rows);
        const LLUUID id = item_id;
        LLHandle<LLView> handle = sAspectMenu;
        menu->setPickCallback([id, handle](S32 index)
                              {
                                  if (SKExplorerPopupList* list = static_cast<SKExplorerPopupList*>(handle.get()))
                                  {
                                      const LLSD value = list->getRows()[index].mValue;
                                      list->hide();
                                      setAspect(id, value["w"].asInteger(), value["h"].asInteger());
                                  }
                              });
        constexpr S32 WIDTH = 90;
        if (above)
        {
            menu->showAbove(spawn, rect, WIDTH, 0, 3);
        }
        else
        {
            menu->showBelow(spawn, rect, WIDTH, 0, 0, 3);
        }
    }
}

//
// SKInventoryPlayer
//

SKInventoryPlayer::SKInventoryPlayer()
:   mKind(KIND_NONE),
    mAspect(1.f),
    mFetching(false),
    mZoomable(false),
    mHoverButton(-1),
    mHoverX(-1),
    mMark(0.f),
    mSoundPlaying(false),
    mAnimMode(-1),
    mAnimStart(0.0),
    mAnimStop(0.0),
    mZoom(1.f),
    mZoomShown(1.f),
    mZoomOrigin(0.5f, 0.5f)
{
}

SKInventoryPlayer::~SKInventoryPlayer()
{
    stop();
}

void SKInventoryPlayer::stop()
{
    if (mAnimMode >= 0 && mAssetID.notNull())
    {
        if (mAnimMode == 0 && isAgentAvatarValid())
        {
            gAgentAvatarp->stopMotion(mAssetID);
        }
        else if (mAnimMode == 1)
        {
            gAgent.sendAnimationRequest(mAssetID, ANIM_REQUEST_STOP);
        }
    }
    mAnimMode = -1;
    if (mSoundLocal && mSoundSource.notNull() && gAudiop)
    {
        if (LLAudioSource* source = gAudiop->findAudioSource(mSoundSource))
        {
            source->stop();
        }
    }
    mSoundSource.setNull();
    mSoundLocal = false;
    mSoundHeard = false;
    mSoundPlaying = false;
}

void SKInventoryPlayer::playLocal(F32 at)
{
    stop();
    if (!gAudiop)
    {
        return;
    }
    mSoundSource.generate();
    gAudiop->triggerSound(mAssetID, gAgentID, 1.f, LLAudioEngine::AUDIO_TYPE_UI, LLVector3d::zero, LLUUID::null, mSoundSource);
    if (at > 0.f)
    {
        // Seeks once playing, or starts there once the sound has loaded.
        if (LLAudioSource* source = gAudiop->findAudioSource(mSoundSource))
        {
            source->play(mAssetID, at);
        }
    }
    mSoundLocal = true;
    mSoundPlaying = true;
    mSoundStart = gAudiop->canSeek() ? at : 0.f;
    mSoundTimer.reset();
}

void SKInventoryPlayer::seekWave(S32 x)
{
    const SKSoundInfo* info = SKInventoryAssets::sound(mAssetID, false);
    if (!info || info->mState != SK_ASSET_READY)
    {
        return;
    }
    const LLRect wave = waveRect();
    mMark = llclamp(info->mLength * (F32)(x - wave.mLeft) / (F32)llmax(1, wave.getWidth()), 0.f, info->mLength);
    if (mSoundPlaying && !mSoundLocal)
    {
        // Our inworld trigger always plays from the start: a click only marks a time.
        return;
    }
    LLAudioSource* source = mSoundPlaying && gAudiop ? gAudiop->findAudioSource(mSoundSource) : nullptr;
    if (source && source->seek(mMark))
    {
        mSoundStart = mMark;
        mSoundTimer.reset();
        return;
    }
    playLocal(mMark);
}

void SKInventoryPlayer::setItem(const LLUUID& item_id, const LLUUID& image_id)
{
    const LLViewerInventoryItem* item = target_item(item_id);
    const LLUUID asset_id = item ? item->getAssetUUID() : LLUUID::null;
    if (item_id == mItemID && image_id == mImageID && asset_id == mAssetID)
    {
        // The same item: only its aspect, kept in the description, may have changed.
        S32 w = 1;
        S32 h = 1;
        if (mKind == KIND_IMAGE && item && item->getType() == LLAssetType::AT_TEXTURE)
        {
            if (mImageID == mAssetID)
            {
                SKInventoryDetails::aspectFor(item, w, h);
                mAspect = (F32)w / (F32)h;
            }
            else
            {
                mAspect = SKInventoryDetails::aspectOf(item->getDescription(), w, h) ? (F32)w / (F32)h : 1.f;
            }
        }
        return;
    }
    stop();
    mItemID = item_id;
    mAssetID = asset_id;
    mImageID = image_id;
    mTexture = nullptr;
    mMaterial = nullptr;
    mMaterialPreview = nullptr;
    mMark = 0.f;
    mZoom = 1.f;
    mZoomShown = 1.f;
    mZoomOrigin.set(0.5f, 0.5f);
    mAspect = 1.f;
    mKind = KIND_NONE;
    S32 w = 1;
    S32 h = 1;
    if (image_id.notNull())
    {
        // A thumbnail, an item's or a folder's, is square unless a texture says otherwise.
        mKind = KIND_IMAGE;
        if (item && item->getType() == LLAssetType::AT_TEXTURE && SKInventoryDetails::aspectOf(item->getDescription(), w, h))
        {
            mAspect = (F32)w / (F32)h;
        }
        return;
    }
    if (!item)
    {
        return;
    }
    switch (item->getType())
    {
        case LLAssetType::AT_TEXTURE:
            mKind = KIND_IMAGE;
            mImageID = asset_id;
            SKInventoryDetails::aspectFor(item, w, h);
            mAspect = (F32)w / (F32)h;
            break;
        case LLAssetType::AT_SOUND:
            mKind = KIND_SOUND;
            break;
        case LLAssetType::AT_ANIMATION:
            mKind = KIND_ANIMATION;
            break;
        case LLAssetType::AT_MATERIAL:
            mKind = KIND_MATERIAL;
            break;
        case LLAssetType::AT_LANDMARK:
            mKind = KIND_LANDMARK;
            break;
        default:
            break;
    }
}

S32 SKInventoryPlayer::height(S32 width) const
{
    switch (mKind)
    {
        case KIND_IMAGE:
            return llmin(MEDIA_HEIGHT, llmax(40, (S32)(width / llmax(mAspect, 0.01f))));
        case KIND_SOUND:
            return WAVE_HEIGHT + BUTTON_GAP + BUTTON_HEIGHT;
        case KIND_ANIMATION:
            return TIMELINE_HEIGHT + BUTTON_GAP + BUTTON_HEIGHT;
        case KIND_MATERIAL:
            return MEDIA_HEIGHT;
        case KIND_LANDMARK:
            return MAP_HEIGHT;
        default:
            return 0;
    }
}

void SKInventoryPlayer::drawMessage(const LLRect& rect, const std::string& text)
{
    static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray_50");
    LLFontGL::getFontSansSerifSmall()->renderUTF8(text, 0, (S32)rect.getCenterX(), (S32)rect.getCenterY(), dim_color.get(),
                                                  LLFontGL::HCENTER, LLFontGL::VCENTER);
}

void SKInventoryPlayer::draw(const LLRect& rect, bool hovered)
{
    mRect = rect;
    mImageRect = LLRect();
    if (!hovered)
    {
        mHoverButton = -1;
        mHoverX = -1;
    }
    LLLocalClipRect clip(rect);
    switch (mKind)
    {
        case KIND_IMAGE:
            drawImage(rect);
            break;
        case KIND_SOUND:
            drawSound(rect);
            break;
        case KIND_ANIMATION:
            drawAnimation(rect);
            break;
        case KIND_MATERIAL:
            drawMaterial(rect);
            break;
        case KIND_LANDMARK:
            drawLandmark(rect);
            break;
        default:
            break;
    }
}

void SKInventoryPlayer::drawImage(const LLRect& rect)
{
    if (mImageID.isNull())
    {
        return;
    }
    if (!mTexture && mFetching)
    {
        // No boost: the texture may be shared with the world, and a boost would shrink it for everyone.
        mTexture = LLViewerTextureManager::getFetchedTexture(mImageID, FTT_DEFAULT, MIPMAP_YES, LLGLTexture::BOOST_NONE);
    }
    // A texture without an aspect tail takes its own width:height once its size arrives.
    if (mImageID == mAssetID && mTexture && mTexture->getFullWidth() > 0)
    {
        S32 w = 1;
        S32 h = 1;
        SKInventoryDetails::aspectFor(target_item(mItemID), w, h);
        mAspect = (F32)w / (F32)h;
    }
    // Fit: min(96% of the width, 92% of the height × aspect) in the Gallery; the whole rect in the details.
    const F32 fit_w = mZoomable ? llmin(rect.getWidth() * 0.96f, rect.getHeight() * 0.92f * mAspect) : llmin((F32)rect.getWidth(), rect.getHeight() * mAspect);
    const S32 w = llmax(1, (S32)fit_w);
    const S32 h = llmax(1, (S32)(fit_w / mAspect));
    mImageRect.setCenterAndSize(rect.getCenterX(), rect.getCenterY(), w, h);
    if (!mTexture)
    {
        gl_rect_2d(mImageRect, LLColor4(0.f, 0.f, 0.f, 0.3f), true);
        return;
    }

    // The zoom eases toward its target, and grows the picture about the pointer's place.
    const F32 dt = llmin(1.f, mZoomTimer.getElapsedTimeAndResetF32() / ZOOM_EASE);
    mZoomShown += (mZoom - mZoomShown) * dt;
    if (fabsf(mZoomShown - mZoom) < 0.001f)
    {
        mZoomShown = mZoom;
    }
    LLRect drawn = mImageRect;
    if (mZoomShown > 1.f)
    {
        const F32 ox = mImageRect.mLeft + mZoomOrigin.mV[VX] * w;
        const F32 oy = mImageRect.mBottom + mZoomOrigin.mV[VY] * h;
        drawn.set((S32)(ox - (ox - mImageRect.mLeft) * mZoomShown), (S32)(oy + (mImageRect.mTop - oy) * mZoomShown),
                  (S32)(ox + (mImageRect.mRight - ox) * mZoomShown), (S32)(oy - (oy - mImageRect.mBottom) * mZoomShown));
    }
    report_drawn(mTexture, drawn.getWidth(), drawn.getHeight());
    // Texel-sharp from 3x.
    const ALSampler sampler = mZoomShown >= 3.f ? ALSamplers::PointClamp : ALSamplers::BilinearClamp;
    draw_image(drawn, mTexture, LLRectf(0.f, 1.f, 1.f, 0.f), sampler);
}

LLRect SKInventoryPlayer::waveRect() const
{
    const S32 area = mKind == KIND_SOUND ? WAVE_HEIGHT : TIMELINE_HEIGHT;
    return LLRect(mRect.mLeft, mRect.mTop, mRect.mRight, mRect.mTop - llmin(area, mRect.getHeight() - BUTTON_HEIGHT - BUTTON_GAP));
}

void SKInventoryPlayer::drawButtons(const LLRect& rect)
{
    static LLPointer<LLUIImage> off = LLUI::getUIImage("PushButton_Off");
    static LLPointer<LLUIImage> over = LLUI::getUIImage("PushButton_Over");
    static LLPointer<LLUIImage> on = LLUI::getUIImage("PushButton_Selected");
    static LLPointer<LLUIImage> play = LLUI::getUIImage("Play_Off");
    static LLPointer<LLUIImage> stop_icon = LLUI::getUIImage("Stop_Off");
    static LLUIColor text_color = LLUIColorTable::instance().getColor("LabelTextColor");
    const S32 half = (rect.getWidth() - BUTTON_GAP) / 2;
    for (S32 i = 0; i < 2; ++i)
    {
        const S32 left = rect.mLeft + i * (half + BUTTON_GAP);
        mButtons[i].set(left, rect.mBottom + BUTTON_HEIGHT, left + half, rect.mBottom);
        const bool active = (mKind == KIND_ANIMATION && mAnimMode == i) ||
                            (mKind == KIND_SOUND && mSoundPlaying && mSoundLocal == (i == 0));
        LLUIImage* image = active ? on.get() : (mHoverButton == i ? over.get() : off.get());
        if (image)
        {
            image->draw(mButtons[i]);
        }
        std::string label = SKInventoryDetails::str(i == 0 ? "player_locally" : "player_inworld");
        if (active)
        {
            label = SKInventoryDetails::str(mKind == KIND_SOUND && i == 1 ? "player_stop_for_me" : "player_stop");
        }
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        const S32 icon = 14;
        const S32 total = icon + 4 + font->getWidth(label);
        const S32 x = mButtons[i].getCenterX() - total / 2;
        LLUIImage* glyph = active ? stop_icon.get() : play.get();
        if (glyph)
        {
            glyph->draw(x, mButtons[i].getCenterY() - icon / 2, icon, icon);
        }
        font->renderUTF8(label, 0, (S32)(x + icon + 4), (S32)mButtons[i].getCenterY(), text_color.get(), LLFontGL::LEFT, LLFontGL::VCENTER);
    }
}

void SKInventoryPlayer::drawSound(const LLRect& rect)
{
    const LLRect wave = waveRect();
    gl_rect_2d(wave, LLColor4(0.f, 0.f, 0.f, 0.35f), true);
    drawButtons(LLRect(rect.mLeft, rect.mBottom + BUTTON_HEIGHT, rect.mRight, rect.mBottom));
    const SKSoundInfo* info = SKInventoryAssets::sound(mAssetID, mFetching);
    if (!info || info->mState == SK_ASSET_LOADING)
    {
        drawMessage(wave, SKInventoryDetails::str("loading"));
        return;
    }
    if (info->mState == SK_ASSET_FAILED || info->mPeaks.empty())
    {
        drawMessage(wave, SKInventoryDetails::str("sound_failed"));
        return;
    }

    // The playhead is the engine's position for a local preview; our inworld copy runs on a clock from 0.
    F32 played = -1.f;
    F32 now_time = mMark;
    if (mSoundPlaying)
    {
        bool ended = false;
        if (mSoundLocal)
        {
            LLAudioSource* source = gAudiop ? gAudiop->findAudioSource(mSoundSource) : nullptr;
            const F32 position = source ? source->getPlayPosition() : -1.f;
            if (!source)
            {
                ended = true;
            }
            else if (position >= 0.f)
            {
                mSoundHeard = true;
                now_time = position;
            }
            else if (mSoundHeard)
            {
                ended = true;
            }
            else
            {
                // Still loading where the engine reports a position; a clock where it reports none.
                now_time = mSoundStart + (gAudiop->canSeek() ? 0.f : mSoundTimer.getElapsedTimeF32());
            }
        }
        else
        {
            now_time = mSoundTimer.getElapsedTimeF32();
        }
        if (ended || now_time >= info->mLength)
        {
            mSoundPlaying = false;
            now_time = mMark;
        }
        else
        {
            played = now_time / llmax(info->mLength, 0.01f);
        }
    }
    const S32 width = wave.getWidth();
    const S32 bars = llmax(1, width / (BAR_WIDTH + BAR_GAP));
    const F32 hover = mHoverX >= wave.mLeft && mHoverX < wave.mRight ? (F32)(mHoverX - wave.mLeft) / (F32)width : -1.f;
    const S32 height = wave.getHeight();
    const S32 mid = wave.mBottom + (S32)(height * (1.f - 0.68f));
    const S32 top_room = wave.mTop - mid;
    const S32 slices = (S32)info->mPeaks.size();
    for (S32 i = 0; i < bars; ++i)
    {
        const F32 at = (F32)i / (F32)bars;
        const S32 s0 = i * slices / bars;
        const S32 s1 = llmax(s0 + 1, (i + 1) * slices / bars);
        F32 peak = 0.f;
        LLVector3 band(0.f, 0.f, 0.f);
        for (S32 s = s0; s < s1 && s < slices; ++s)
        {
            peak = llmax(peak, info->mPeaks[s]);
            band += info->mBands[s];
        }
        band /= (F32)(s1 - s0);
        const S32 state = (played >= 0.f && at < played) ? 2 : (hover >= 0.f && at < hover) ? 1 : 0;
        const S32 x = wave.mLeft + i * (BAR_WIDTH + BAR_GAP);
        const F32 bar_height = peak * top_room * 0.95f;
        F32 y = (F32)mid;
        for (S32 k = 0; k < 3; ++k)
        {
            const F32 part = bar_height * band.mV[k];
            gl_rect_2d(x, (S32)(y + part), x + BAR_WIDTH, (S32)y, hex(SHADES[state][k]), true);
            y += part;
        }
        // A faded mirror below, the total level in the mids shade.
        gl_rect_2d(x, mid - 1, x + BAR_WIDTH, mid - 1 - (S32)(peak * (mid - wave.mBottom) * 0.9f), hex(SHADES[state][1], 0x55 / 255.f), true);
    }
    draw_chip(wave.mLeft + 2, wave.mTop - 2, llformat("%.1f / %.1fs", now_time, info->mLength));
}

F32 SKInventoryPlayer::animTime(F64 now, F32& weight)
{
    const SKAnimInfo* info = SKInventoryAssets::animation(mAssetID, false);
    weight = 0.f;
    if (!info || info->mState != SK_ASSET_READY || mAnimMode < 0 && mAnimStop <= 0.0)
    {
        return -1.f;
    }
    const F32 elapsed = (F32)(now - mAnimStart);
    F32 t = elapsed;
    if (info->mLoop && t > info->mLoopOut)
    {
        t = info->mLoopIn + fmodf(t - info->mLoopOut, llmax(0.1f, info->mLoopOut - info->mLoopIn));
    }
    else if (!info->mLoop && t > info->mDuration)
    {
        return -1.f;
    }
    // The playhead's strength follows the motion's weight: in over the ease in, out over the ease out.
    weight = info->mEaseIn > 0.f ? llmin(1.f, elapsed / info->mEaseIn) : 1.f;
    if (!info->mLoop && info->mEaseOut > 0.f)
    {
        weight = llmin(weight, (info->mDuration - t) / info->mEaseOut);
    }
    if (mAnimStop > 0.0)
    {
        const F32 out = info->mEaseOut > 0.f ? 1.f - (F32)(now - mAnimStop) / info->mEaseOut : 0.f;
        if (out <= 0.f)
        {
            return -1.f;
        }
        weight = llmin(weight, out);
    }
    weight = llmax(0.f, weight);
    return t;
}

void SKInventoryPlayer::drawAnimation(const LLRect& rect)
{
    static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray_50");
    const LLRect line = waveRect();
    gl_rect_2d(line, LLColor4(0.f, 0.f, 0.f, 0.35f), true);
    drawButtons(LLRect(rect.mLeft, rect.mBottom + BUTTON_HEIGHT, rect.mRight, rect.mBottom));
    const SKAnimInfo* info = SKInventoryAssets::animation(mAssetID, mFetching);
    if (!info || info->mState == SK_ASSET_LOADING)
    {
        drawMessage(line, SKInventoryDetails::str("loading"));
        return;
    }
    if (info->mState == SK_ASSET_FAILED || info->mDuration <= 0.f)
    {
        drawMessage(line, SKInventoryDetails::str("animation_failed"));
        return;
    }
    const F32 len = info->mDuration;
    auto X = [&](F32 t) { return line.mLeft + 6 + (S32)((line.getWidth() - 12) * llclamp(t / len, 0.f, 1.f)); };
    const S32 top = line.mTop - 8;
    const S32 base = line.mBottom + 15;
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (info->mLoop)
    {
        gl_rect_2d(X(info->mLoopIn), line.mTop - 4, X(info->mLoopOut), line.mBottom + 14, LLColor4(ACCENT.mV[0], ACCENT.mV[1], ACCENT.mV[2], 0.18f), true);
        font->renderUTF8(SKInventoryDetails::str("anim_loop"), 0, (S32)(X(info->mLoopIn) + 4), (S32)(line.mTop - 6), ACCENT,
                         LLFontGL::LEFT, LLFontGL::TOP);
    }
    // The weight: up over the ease in, held, down over the ease out.
    const LLColor4 grey = hex(0x9fa2a3);
    const S32 x1 = X(llmin(info->mEaseIn, len / 2.f));
    const S32 x2 = X(llmax(len - info->mEaseOut, len / 2.f));
    gl_line_2d(X(0.f), base, x1, top, grey);
    gl_line_2d(x1, top, x2, top, grey);
    gl_line_2d(x2, top, X(len), base, grey);
    font->renderUTF8(std::string("0s"), 0, (S32)X(0.f), (S32)(line.mBottom + 2), dim_color.get(), LLFontGL::LEFT, LLFontGL::BOTTOM);
    font->renderUTF8(seconds(len), 0, (S32)X(len), (S32)(line.mBottom + 2), dim_color.get(), LLFontGL::RIGHT, LLFontGL::BOTTOM);

    F32 weight = 0.f;
    const F32 t = animTime(LLFrameTimer::getTotalSeconds(), weight);
    if (t < 0.f)
    {
        if (mAnimMode < 0)
        {
            mAnimStop = 0.0;
        }
        else if (!info->mLoop)
        {
            // Played to its end.
            mAnimMode = -1;
            mAnimStop = 0.0;
        }
        return;
    }
    const S32 x = X(t);
    gl_rect_2d(x - 1, line.mTop - 3, x + 1, line.mBottom + 16, LLColor4(ACCENT.mV[0], ACCENT.mV[1], ACCENT.mV[2], weight), true);
}

void SKInventoryPlayer::drawMaterial(const LLRect& rect)
{
    if (!mMaterial && mFetching && mAssetID.notNull())
    {
        mMaterial = static_cast<LLFetchedGLTFMaterial*>(gGLTFMaterialList.getMaterial(mAssetID));
    }
    if (mMaterial && mMaterial->isLoaded() && !mMaterialPreview)
    {
        // The manager keeps no cache: one preview per material shown.
        mMaterialPreview = gGLTFMaterialPreviewMgr.getPreview(mMaterial);
    }
    const S32 side = llmin(rect.getWidth(), rect.getHeight());
    mImageRect.setCenterAndSize(rect.getCenterX(), rect.getCenterY(), side, side);
    if (!mMaterialPreview)
    {
        drawMessage(rect, (mMaterial && !mMaterial->isFetching() && !mMaterial->isLoaded()) ? SKInventoryDetails::str("material_failed")
                                                                                              : SKInventoryDetails::str("loading"));
        return;
    }
    draw_image(mImageRect, mMaterialPreview, LLRectf(0.f, 1.f, 1.f, 0.f), ALSamplers::BilinearClamp);
}

void SKInventoryPlayer::drawLandmark(const LLRect& rect)
{
    LLLandmark* landmark = SKInventoryAssets::landmark(mAssetID, mFetching);
    LLVector3d pos;
    if (!landmark || !landmark->getGlobalPos(pos))
    {
        drawMessage(rect, SKInventoryDetails::str("landmark_loads"));
        return;
    }
    const U32 grid_x = (U32)(pos.mdV[VX] / REGION_WIDTH_METERS);
    const U32 grid_y = (U32)(pos.mdV[VY] / REGION_WIDTH_METERS);
    if (!mTexture)
    {
        mTexture = LLWorldMap::getInstance()->getObjectsTile(grid_x, grid_y, 1);
    }
    const S32 side = llmin(rect.getWidth(), rect.getHeight());
    mImageRect.setCenterAndSize(rect.getCenterX(), rect.getCenterY(), side, side);
    if (!mTexture || mTexture->getDiscardLevel() < 0)
    {
        if (mTexture)
        {
            report_drawn(mTexture, side, side);
        }
        drawMessage(rect, SKInventoryDetails::str("map_loading"));
        return;
    }
    report_drawn(mTexture, side, side);
    draw_image(mImageRect, mTexture, LLRectf(0.f, 1.f, 1.f, 0.f), ALSamplers::BilinearClamp);
    gl_rect_2d(mImageRect, LLColor4(0.f, 0.f, 0.f, 0.35f), false);

    // The pin: a blue drop with a white dot.
    const F32 lx = (F32)fmod(pos.mdV[VX], (F64)REGION_WIDTH_METERS) / REGION_WIDTH_METERS;
    const F32 ly = (F32)fmod(pos.mdV[VY], (F64)REGION_WIDTH_METERS) / REGION_WIDTH_METERS;
    const S32 px = mImageRect.mLeft + (S32)(lx * side);
    const S32 py = mImageRect.mBottom + (S32)(ly * side);
    gGL.getTextureSlot(0)->unbind();
    gGL.color4fv(ACCENT.mV);
    gl_circle_2d((F32)px, (F32)(py + 9), 5.f, 16, true);
    gl_triangle_2d(px - 5, py + 9, px + 5, py + 9, px, py, ACCENT, true);
    gGL.color4fv(LLColor4::white.mV);
    gl_circle_2d((F32)px, (F32)(py + 9), 2.f, 8, true);

    if (LLSimInfo* info = LLWorldMap::getInstance()->simInfoFromPosGlobal(pos))
    {
        draw_chip(mImageRect.mLeft + 3, mImageRect.mTop - 3, info->getName());
    }
}

void SKInventoryPlayer::pressButton(S32 index)
{
    make_ui_sound("UISndClick");
    if (mKind == KIND_SOUND)
    {
        // Each button toggles: locally from the marked time, inworld from the start.
        if (index == 0)
        {
            if (mSoundPlaying && mSoundLocal)
            {
                stop();
            }
            else
            {
                playLocal(mMark);
            }
            return;
        }
        if (mSoundPlaying && !mSoundLocal)
        {
            // Stops only our copy: the server's trigger plays on for everyone else.
            if (gAudiop)
            {
                gAudiop->stopTriggeredSounds(mAssetID, gAgentID);
            }
            stop();
            return;
        }
        stop();
        send_sound_trigger(mAssetID, 1.f);
        mSoundPlaying = true;
        mSoundTimer.reset();
        return;
    }
    if (mKind != KIND_ANIMATION)
    {
        return;
    }
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (mAnimMode == index)
    {
        // A second press stops; the playhead runs on through the ease out.
        stop();
        mAnimStop = now;
        return;
    }
    stop();
    mAnimStop = 0.0;
    if (index == 0)
    {
        if (!isAgentAvatarValid())
        {
            return;
        }
        gAgentAvatarp->startMotion(mAssetID);
    }
    else
    {
        gAgent.sendAnimationRequest(mAssetID, ANIM_REQUEST_START);
    }
    mAnimMode = index;
    mAnimStart = now;
}

bool SKInventoryPlayer::handleMouseDown(S32 x, S32 y)
{
    if (!mRect.pointInRect(x, y))
    {
        return false;
    }
    if (mKind == KIND_SOUND || mKind == KIND_ANIMATION)
    {
        for (S32 i = 0; i < 2; ++i)
        {
            if (mButtons[i].pointInRect(x, y))
            {
                pressButton(i);
                return true;
            }
        }
        if (mKind == KIND_SOUND && waveRect().pointInRect(x, y))
        {
            seekWave(x);
            mWaveDrag = true;
        }
    }
    return true;
}

bool SKInventoryPlayer::handleDoubleClick(S32 x, S32 y)
{
    if (mZoomable && mKind == KIND_IMAGE && mRect.pointInRect(x, y))
    {
        mZoom = 1.f;
        return true;
    }
    return handleMouseDown(x, y);
}

void SKInventoryPlayer::handleHover(S32 x, S32 y)
{
    // Only the waveform or timeline itself previews a seek, not the buttons under it.
    mHoverX = (mWaveDrag || waveRect().pointInRect(x, y)) ? x : -1;
    mHoverButton = -1;
    if (mWaveDrag)
    {
        // A drag along the waveform seeks as it goes.
        mWaveDrag = mKind == KIND_SOUND && gViewerWindow->getLeftMouseDown();
        const LLRect wave = waveRect();
        if (mWaveDrag && x >= wave.mLeft && x < wave.mRight)
        {
            seekWave(x);
        }
    }
    for (S32 i = 0; i < 2; ++i)
    {
        if ((mKind == KIND_SOUND || mKind == KIND_ANIMATION) && mButtons[i].pointInRect(x, y))
        {
            mHoverButton = i;
        }
    }
    if (mZoomable && mKind == KIND_IMAGE && mRect.notEmpty())
    {
        // The middle 80% of the stage spans the whole picture.
        if (mZoom > 1.f)
        {
            const F32 fx = (F32)(x - mRect.mLeft) / (F32)llmax(1, mRect.getWidth());
            const F32 fy = (F32)(y - mRect.mBottom) / (F32)llmax(1, mRect.getHeight());
            mZoomOrigin.set(llclamp((fx - 0.1f) / 0.8f, 0.f, 1.f), llclamp((fy - 0.1f) / 0.8f, 0.f, 1.f));
        }
        if (mRect.pointInRect(x, y))
        {
            gViewerWindow->getWindow()->setCursor(mZoom > 1.f ? UI_CURSOR_TOOLZOOMOUT : UI_CURSOR_TOOLZOOMIN);
        }
    }
}

bool SKInventoryPlayer::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (!mZoomable || mKind != KIND_IMAGE || !mRect.pointInRect(x, y))
    {
        return false;
    }
    // A wheel notch is about 100 in a browser's deltaY.
    mZoom = llclamp(mZoom * expf(-(F32)clicks * 100.f * 0.0015f), 1.f, ZOOM_MAX);
    handleHover(x, y);
    return true;
}

bool SKInventoryPlayer::getToolTip(S32 x, S32 y, std::string& tip) const
{
    for (S32 i = 0; i < 2; ++i)
    {
        if ((mKind == KIND_SOUND || mKind == KIND_ANIMATION) && mButtons[i].pointInRect(x, y))
        {
            const bool stop_for_me = mKind == KIND_SOUND && i == 1 && mSoundPlaying && !mSoundLocal;
            tip = SKInventoryDetails::str(stop_for_me ? "tip_sound_stop_for_me"
                                        : mKind == KIND_SOUND ? (i == 0 ? "tip_sound_local" : "tip_sound_inworld")
                                                              : (i == 0 ? "tip_anim_local" : "tip_anim_inworld"));
            return true;
        }
    }
    return false;
}

void SKInventoryPlayer::mouseLeave()
{
    mHoverX = -1;
    mHoverButton = -1;
}

//
// SKInventoryDetailsView
//

SKInventoryDetailsView::SKInventoryDetailsView(const Params& p)
:   LLUICtrl(p),
    mThumbnail(false),
    mMediaHeight(0),
    mContentHeight(0),
    mDirty(true),
    mPending(false),
    mRevision(0),
    mLaidWidth(-1),
    mHoverLine(-1),
    mHoverMedia(false),
    mFetching(false),
    mScrollbar(nullptr)
{
    LLScrollbar::Params sbparams;
    sbparams.name("details_scrollbar");
    sbparams.rect(LLRect(getRect().getWidth() - LLScrollbar::THIN_THICKNESS, getRect().getHeight(), getRect().getWidth(), 0));
    sbparams.orientation(LLScrollbar::VERTICAL);
    sbparams.doc_size(0);
    sbparams.doc_pos(0);
    sbparams.page_size(1);
    sbparams.follows.flags(FOLLOWS_RIGHT | FOLLOWS_TOP | FOLLOWS_BOTTOM);
    sbparams.visible(false);
    sbparams.thin(true);
    mScrollbar = LLUICtrlFactory::create<LLScrollbar>(sbparams);
    addChild(mScrollbar);
}

SKInventoryDetailsView::~SKInventoryDetailsView()
{
    mNameConnection.disconnect();
}

void SKInventoryDetailsView::onVisibilityChange(bool new_visibility)
{
    if (!new_visibility)
    {
        mPlayer.stop();
        mThumb.stop();
    }
    LLUICtrl::onVisibilityChange(new_visibility);
}

void SKInventoryDetailsView::setSelection(const uuid_vec_t& ids, const std::string& folder_path)
{
    const LLUUID item_id = ids.size() == 1 ? ids.front() : LLUUID::null;
    if (ids == mSelection && folder_path == mFolderPath)
    {
        return;
    }
    mSelection = ids;
    mFolderPath = folder_path;
    if (item_id != mItemID)
    {
        mItemID = item_id;
        // Asset fetches wait for the selection to settle.
        mSettle.reset();
        mFetching = false;
        mPlayer.setFetching(false);
        mScrollbar->setDocPos(0);
        SKInventoryDetails::hideAspectMenu();
    }
    mDirty = true;
}

void SKInventoryDetailsView::addHeading(const std::string& key)
{
    Line line;
    line.mKind = Line::LINE_HEADING;
    line.mLabel = SKInventoryDetails::str(key);
    LLStringUtil::toUpper(line.mLabel);
    mLines.push_back(line);
}

void SKInventoryDetailsView::addRow(const std::string& label_key, const std::string& value, const std::string& url, const std::string& tip)
{
    if (value.empty())
    {
        return; // rows drop when empty
    }
    Line line;
    line.mLabel = SKInventoryDetails::str(label_key);
    line.mValue.push_back({ value });
    line.mURL = url;
    line.mToolTip = tip;
    mLines.push_back(line);
}

void SKInventoryDetailsView::addPermsRow(const std::string& label_key, U32 mask)
{
    Line line;
    line.mLabel = SKInventoryDetails::str(label_key);
    if ((mask & PERM_ITEM_UNRESTRICTED) == PERM_ITEM_UNRESTRICTED)
    {
        line.mValue.push_back({ SKInventoryDetails::str("perm_full") });
    }
    else
    {
        // Absent ones struck through.
        line.mValue.push_back({ SKInventoryDetails::str("perm_modify"), false, !(mask & PERM_MODIFY) });
        line.mValue.push_back({ SKInventoryDetails::str("perm_copy"), false, !(mask & PERM_COPY) });
        line.mValue.push_back({ SKInventoryDetails::str("perm_transfer"), false, !(mask & PERM_TRANSFER) });
    }
    mLines.push_back(line);
}

void SKInventoryDetailsView::addTypeRows(LLViewerInventoryItem* item, LLViewerInventoryItem* target)
{
    const LLUUID asset_id = target->getAssetUUID();
    switch (target->getType())
    {
        case LLAssetType::AT_TEXTURE:
        {
            addHeading("section_image");
            if (mFetching && (!mInfoTexture || mInfoTexture->getID() != asset_id))
            {
                mInfoTexture = LLViewerTextureManager::getFetchedTexture(asset_id, FTT_DEFAULT, MIPMAP_YES, LLGLTexture::BOOST_NONE);
            }
            LLViewerFetchedTexture* texture = mInfoTexture && mInfoTexture->getID() == asset_id ? mInfoTexture.get() : nullptr;
            if (texture && texture->getFullWidth() > 0)
            {
                addRow("row_resolution", llformat("%d\xC3\x97%d", texture->getFullWidth(), texture->getFullHeight()));
                addRow("row_channels", SKInventoryDetails::str(texture->getComponents() == 4 ? "channels_rgba" : "channels_rgb"));
            }
            else
            {
                addRow("row_resolution", SKInventoryDetails::str("loading"));
                mPending = true;
            }
            S32 w = 1;
            S32 h = 1;
            SKInventoryDetails::aspectFor(target, w, h);
            std::string why;
            const bool can = SKInventoryDetails::canSetAspect(target->getUUID(), &why);
            addRow("row_aspect", llformat("%d:%d \xE2\x96\xBE", w, h), can ? "aspect:" : std::string(),
                   can ? SKInventoryDetails::str("tip_aspect") : why);
            break;
        }
        case LLAssetType::AT_SOUND:
        {
            addHeading("section_sound");
            const SKSoundInfo* info = SKInventoryAssets::sound(asset_id, mFetching);
            if (info && info->mState == SK_ASSET_READY)
            {
                addRow("row_length", seconds(info->mLength));
                LLStringUtil::format_map_t args;
                args["[RATE]"] = llformat("%.1f", info->mRate / 1000.f);
                args["[CHANNELS]"] = SKInventoryDetails::str(info->mChannels == 1 ? "sound_mono" : "sound_stereo");
                addRow("row_format", format_args(SKInventoryDetails::str("sound_format"), args));
            }
            else
            {
                addRow("row_length", SKInventoryDetails::str(info && info->mState == SK_ASSET_FAILED ? "sound_failed" : "loading"));
                mPending = !info || info->mState == SK_ASSET_LOADING;
            }
            break;
        }
        case LLAssetType::AT_ANIMATION:
        {
            addHeading("section_animation");
            const SKAnimInfo* info = SKInventoryAssets::animation(asset_id, mFetching);
            if (!info || info->mState != SK_ASSET_READY)
            {
                addRow("row_length", SKInventoryDetails::str(info && info->mState == SK_ASSET_FAILED ? "animation_failed" : "loading"));
                mPending = !info || info->mState == SK_ASSET_LOADING;
                break;
            }
            addRow("row_length", seconds(info->mDuration));
            std::string loops = SKInventoryDetails::str("anim_loops_no");
            if (info->mLoop)
            {
                const bool whole = info->mLoopIn <= 0.f && info->mLoopOut >= info->mDuration;
                LLStringUtil::format_map_t args;
                args["[IN]"] = seconds(info->mLoopIn);
                args["[OUT]"] = seconds(info->mLoopOut);
                loops = whole ? SKInventoryDetails::str("anim_loops_whole") : format_args(SKInventoryDetails::str("anim_loops_range"), args);
            }
            addRow("row_loops", loops);
            addRow("row_priority", llformat("%d", info->mPriority) + (info->mPriority >= 5 ? " " + SKInventoryDetails::str("anim_overrides") : std::string()));
            addRow("row_ease", seconds(info->mEaseIn) + " / " + seconds(info->mEaseOut));
            std::string regions;
            for (const std::string& region : info->mRegions)
            {
                regions += (regions.empty() ? "" : ", ") + SKInventoryDetails::str("region_" + region);
            }
            addRow("row_joints", llformat("%d", info->mJoints) + (regions.empty() ? std::string() : " \xC2\xB7 " + regions));
            if (info->mHandPose != LLHandMotion::HAND_POSE_RELAXED)
            {
                addRow("row_hand_pose", LLHandMotion::getHandPoseName((LLHandMotion::eHandPose)info->mHandPose));
            }
            std::string emote = info->mEmote;
            if (emote.rfind("express_", 0) == 0)
            {
                emote = emote.substr(8);
            }
            LLStringUtil::replaceChar(emote, '_', ' ');
            addRow("row_expression", emote);
            break;
        }
        case LLAssetType::AT_MATERIAL:
        {
            addHeading("section_material");
            LLFetchedGLTFMaterial* material = mFetching ? static_cast<LLFetchedGLTFMaterial*>(gGLTFMaterialList.getMaterial(asset_id)) : nullptr;
            if (!material || !material->isLoaded())
            {
                addRow("row_base_colour", SKInventoryDetails::str(material && !material->isFetching() ? "material_failed" : "loading"));
                mPending = !material || material->isFetching();
                break;
            }
            const LLColor4& base = material->mBaseColor;
            addRow("row_base_colour", llformat("(%d, %d, %d)", ll_round(base.mV[VRED] * 255.f), ll_round(base.mV[VGREEN] * 255.f),
                                               ll_round(base.mV[VBLUE] * 255.f)));
            mLines.back().mSwatch = LLColor4(base.mV[VRED], base.mV[VGREEN], base.mV[VBLUE], 1.f);
            addRow("row_metallic", llformat("%.2f", material->mMetallicFactor));
            addRow("row_roughness", llformat("%.2f", material->mRoughnessFactor));
            const char* alpha = material->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND ? "alpha_blend"
                              : material->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK ? "alpha_mask" : "alpha_opaque";
            addRow("row_alpha_mode", SKInventoryDetails::str(alpha));
            if (material->mDoubleSided)
            {
                addRow("row_double_sided", SKInventoryDetails::str("yes"));
            }
            std::string textures;
            const std::pair<LLGLTFMaterial::TextureInfo, const char*> slots[] = {
                { LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR, "tex_base" },
                { LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL, "tex_normal" },
                { LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS, "tex_mr" },
                { LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE, "tex_emissive" },
            };
            for (const auto& slot : slots)
            {
                if (material->mTextureId[slot.first].notNull())
                {
                    textures += (textures.empty() ? "" : ", ") + SKInventoryDetails::str(slot.second);
                }
            }
            addRow("row_textures", textures);
            break;
        }
        case LLAssetType::AT_LANDMARK:
        {
            addHeading("section_landmark");
            LLLandmark* landmark = SKInventoryAssets::landmark(asset_id, mFetching);
            LLVector3d pos;
            if (!landmark || !landmark->getGlobalPos(pos))
            {
                addRow("row_location", SKInventoryDetails::str("landmark_loads"), std::string(), SKInventoryDetails::str("tip_landmark_loads"));
                mPending = true;
                break;
            }
            std::string parcel, region, rating;
            S32 hx = 0, hy = 0, hz = 0;
            parse_location(target->getDescription(), parcel, region, hx, hy, hz, rating);
            const S32 x = (S32)fmod(pos.mdV[VX], (F64)REGION_WIDTH_METERS);
            const S32 y = (S32)fmod(pos.mdV[VY], (F64)REGION_WIDTH_METERS);
            const S32 z = (S32)pos.mdV[VZ];
            LLSimInfo* sim = LLWorldMap::getInstance()->simInfoFromPosGlobal(pos);
            if (sim)
            {
                region = sim->getName();
                rating = sim->getAccessString();
            }
            else
            {
                const U16 gx = (U16)(pos.mdV[VX] / REGION_WIDTH_METERS);
                const U16 gy = (U16)(pos.mdV[VY] / REGION_WIDTH_METERS);
                static std::set<U32> requested;
                if (requested.insert(((U32)gx << 16) | gy).second)
                {
                    LLWorldMapMessage::getInstance()->sendMapBlockRequest(gx, gy, gx, gy, true);
                }
                mPending = true;
            }
            addRow("row_region", region);
            addRow("row_parcel", parcel);
            addRow("row_position", llformat("%d, %d, %d", x, y, z));
            addRow("row_maturity", rating);
            if (!region.empty())
            {
                const std::string path = llformat("%s/%d/%d/%d", region.c_str(), x, y, z);
                addRow("row_slurl", path, "secondlife:///app/teleport/" + LLURI::escape(region) + llformat("/%d/%d/%d", x, y, z));
            }
            break;
        }
        case LLAssetType::AT_LSL_TEXT:
        {
            // §3: the runtime field, else SLua by its marks, else LSL.
            const std::string& runtime = target->getRuntime();
            std::string label;
            std::string tip;
            if (runtime == "mono")
            {
                label = SKInventoryDetails::str("runtime_mono");
            }
            else if (runtime == "lsl2" || runtime == "lso")
            {
                label = SKInventoryDetails::str("runtime_lso");
            }
            else if (runtime == "lsl-luau")
            {
                label = SKInventoryDetails::str("runtime_lsl_luau");
            }
            else if (runtime == "luau")
            {
                label = SKInventoryDetails::str("runtime_slua");
            }
            else if (!runtime.empty())
            {
                label = runtime;
            }
            else
            {
                const std::string& desc = target->getDescription();
                std::string name = target->getName();
                LLStringUtil::toLower(name);
                const bool slua = (desc.size() >= 11 && desc.compare(desc.size() - 11, 11, "SLua script") == 0)
                                  || (name.size() >= 5 && name.compare(name.size() - 5, 5, ".slua") == 0);
                label = SKInventoryDetails::str(slua ? "runtime_slua" : "runtime_lsl");
                if (!slua)
                {
                    tip = SKInventoryDetails::str("tip_runtime_unknown");
                }
            }
            addRow("row_runtime", label, std::string(), tip);
            const SKExperienceInfo* xp = SKInventoryAssets::experience(target->getUUID(), mFetching);
            if (!xp || xp->mState == SK_ASSET_LOADING)
            {
                addRow("row_experience", SKInventoryDetails::str("loading"));
                mPending = true;
            }
            else if (xp->mNoCapability)
            {
                addRow("row_experience", SKInventoryDetails::str("experience_no_cap"));
            }
            else if (xp->mExperienceID.notNull())
            {
                const LLSD& experience = LLExperienceCache::instance().get(xp->mExperienceID);
                std::string name = experience.has(LLExperienceCache::NAME) ? experience[LLExperienceCache::NAME].asString() : std::string();
                if (name.empty())
                {
                    name = SKInventoryDetails::str("loading");
                    LLExperienceCache::instance().get(xp->mExperienceID, [](const LLSD&) {});
                    mPending = true;
                }
                addRow("row_experience", name, "secondlife:///app/experience/" + xp->mExperienceID.asString() + "/profile");
            }
            break;
        }
        default:
            break;
    }
}

void SKInventoryDetailsView::rebuild()
{
    mDirty = false;
    mPending = false;
    mRevision = SKInventoryAssets::revision();
    mLines.clear();
    mMessage.clear();
    mThumbnail = false;
    mThumbBelow = false;

    if (mSelection.empty())
    {
        mMessage = SKInventoryDetails::str("no_selection");
    }
    else if (mSelection.size() > 1)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", (S32)mSelection.size());
        mMessage = format_args(SKInventoryDetails::str("n_selected"), args);
    }
    LLViewerInventoryItem* item = mItemID.notNull() ? gInventory.getItem(mItemID) : nullptr;
    LLViewerInventoryItem* target = item ? target_item(mItemID) : nullptr;
    // A link with no target: its name and type only, nothing read as if the link were the asset.
    const bool broken = item && item->getIsLinkType() && target == item;
    if (!item || !target)
    {
        mPlayer.setItem(LLUUID::null);
        mInfoTexture = nullptr;
        LLViewerInventoryCategory* cat = mItemID.notNull() ? gInventory.getCategory(mItemID) : nullptr;
        if (!cat || !mMessage.empty())
        {
            mThumb.setItem(LLUUID::null);
            mLaidWidth = -1;
            return;
        }
        // A folder: its thumbnail on top when it has one, its name, type and what it holds.
        mThumbnail = cat->getThumbnailUUID().notNull();
        mThumb.setItem(mThumbnail ? mItemID : LLUUID::null, cat->getThumbnailUUID());
        mThumb.setFetching(mFetching);
        SKInventoryRow row;
        SKInventoryFlatView::readRow(mItemID, SKInventoryDetails::labels(), row);
        Line name;
        name.mKind = Line::LINE_NAME;
        name.mValue.push_back({ cat->getName() });
        mLines.push_back(name);
        addRow("row_folder", mFolderPath);
        addRow("row_type", row.mType);
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(mItemID, cats, items);
        if (cats || items)
        {
            LLStringUtil::format_map_t args;
            args["[FOLDERS]"] = llformat("%d", cats ? (S32)cats->size() : 0);
            args["[ITEMS]"] = llformat("%d", items ? (S32)items->size() : 0);
            addRow("row_contents", format_args(SKInventoryDetails::str("contents_counts"), args));
        }
        mLaidWidth = -1;
        return;
    }

    SKInventoryRow row;
    SKInventoryFlatView::readRow(mItemID, SKInventoryDetails::labels(), row);
    // A thumbnail goes on top, and the player moves below the rows.
    const LLUUID thumbnail = item->getThumbnailUUID().notNull() ? item->getThumbnailUUID() : target->getThumbnailUUID();
    mThumbnail = thumbnail.notNull();
    // The asset's own preview leads wherever its type has one; the thumbnail then follows below the rows.
    mThumbBelow = mThumbnail && !broken && has_asset_preview(target->getType());
    if (broken)
    {
        mThumbnail = false;
        mThumb.setItem(LLUUID::null);
        mPlayer.setItem(LLUUID::null);
        Line name;
        name.mKind = Line::LINE_NAME;
        name.mValue.push_back({ item->getName() });
        mLines.push_back(name);
        addRow("row_type", row.mType + " " + SKInventoryDetails::str("link_suffix"));
        mLaidWidth = -1;
        return;
    }
    mThumb.setItem(mThumbnail ? mItemID : LLUUID::null, thumbnail);
    mThumb.setFetching(mFetching);
    mPlayer.setItem(mItemID);
    mPlayer.setFetching(mFetching);

    Line name;
    name.mKind = Line::LINE_NAME;
    name.mValue.push_back({ item->getName() });
    mLines.push_back(name);

    addRow("row_folder", mFolderPath);
    std::string type = row.mType;
    if (item->getIsLinkType())
    {
        type += " " + SKInventoryDetails::str("link_suffix");
    }
    addRow("row_type", type);
    const LLUUID creator = target->getCreatorUUID();
    if (creator.notNull())
    {
        LLAvatarName av_name;
        std::string creator_name;
        if (LLAvatarNameCache::get(creator, &av_name))
        {
            creator_name = av_name.getCompleteName();
        }
        else
        {
            creator_name = SKInventoryDetails::str("loading");
            if (!mNameConnection.connected())
            {
                mNameConnection = LLAvatarNameCache::get(creator, [this](const LLUUID&, const LLAvatarName&) { mDirty = true; });
            }
        }
        addRow("row_creator", creator_name, "secondlife:///app/agent/" + creator.asString() + "/about");
    }
    addRow("row_acquired", row.mAcquiredLabel);
    const LLPermissions& perm = target->getPermissions();
    addPermsRow("row_you", perm.getMaskOwner());
    addPermsRow("row_next_owner", perm.getMaskNextOwner());
    if (row.mIsWorn)
    {
        addRow("row_worn", row.mWornOn.empty() ? SKInventoryDetails::str("yes") : row.mWornOn);
    }
    if (target->getType() == LLAssetType::AT_OBJECT && isAgentAvatarValid())
    {
        // The low byte of the flags; none and Right Hand are both where an object goes by default.
        const S32 point = (S32)(target->getFlags() & 0xff);
        if (point != 0 && point != 6)
        {
            auto it = gAgentAvatarp->mAttachmentPoints.find(point);
            if (it != gAgentAvatarp->mAttachmentPoints.end() && it->second)
            {
                addRow("row_attaches_to", LLTrans::getString(it->second->getName()));
            }
        }
    }
    std::string parcel, region, rating;
    S32 hx = 0, hy = 0, hz = 0;
    const bool location_text = target->getType() == LLAssetType::AT_LANDMARK
                               && parse_location(target->getDescription(), parcel, region, hx, hy, hz, rating);
    if (!location_text)
    {
        addRow("row_description", item->getDescription());
    }

    addTypeRows(item, target);

    if (mThumbBelow)
    {
        // The asset's preview took the top; its thumbnail has a section of its own below.
        addHeading("section_thumbnail");
        Line thumb;
        thumb.mKind = Line::LINE_PLAYER;
        mLines.push_back(thumb);
    }
    mLaidWidth = -1;
}

void SKInventoryDetailsView::layout()
{
    const S32 width = getRect().getWidth();
    mLaidWidth = width;
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifBold();
    const S32 line_height = (S32)font->getLineHeight();
    // Laid out once without the bar; if the content then scrolls, again in the width beside it.
    const S32 inner = llmax(40, width - 2 * PAD - (mScrollbar->getVisible() ? LLScrollbar::THIN_THICKNESS : 0));
    const S32 value_width = llmax(40, inner - LABEL_WIDTH);

    S32 top = PAD;
    mMediaHeight = 0;
    if (!mItemID.isNull() && mMessage.empty())
    {
        if (mThumbnail || mPlayer.getKind() != SKInventoryPlayer::KIND_NONE)
        {
            mMediaHeight = (mThumbnail && !mThumbBelow) ? MEDIA_HEIGHT : mPlayer.height(inner);
            top += mMediaHeight + PAD;
        }
    }
    auto wrap = [](const LLFontGL* f, const std::string& text, S32 w, std::vector<std::string>& out)
    {
        out.clear();
        std::string rest = text;
        while (!rest.empty())
        {
            S32 bytes = f->maxDrawableBytes(rest, (F32)w, S32_MAX, LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
            if (bytes <= 0)
            {
                bytes = (S32)rest.size();
            }
            const size_t nl = rest.find('\n');
            if (nl != std::string::npos && (S32)nl < bytes)
            {
                bytes = (S32)nl + 1;
            }
            std::string part = rest.substr(0, bytes);
            LLStringUtil::trimTail(part);
            out.push_back(part);
            rest = rest.substr(bytes);
            LLStringUtil::trimHead(rest);
        }
    };
    for (Line& line : mLines)
    {
        line.mTop = top;
        switch (line.mKind)
        {
            case Line::LINE_NAME:
                wrap(bold, line.mValue.front().mText, inner, line.mWrapped);
                line.mHeight = (S32)line.mWrapped.size() * (S32)bold->getLineHeight() + ROW_GAP * 2;
                break;
            case Line::LINE_HEADING:
                line.mHeight = line_height + 10;
                break;
            case Line::LINE_PLAYER:
                line.mHeight = (mThumbBelow ? mThumb : mPlayer).height(inner) + PAD;
                break;
            default:
            {
                std::string joined;
                for (const Segment& segment : line.mValue)
                {
                    joined += (joined.empty() ? "" : " ") + segment.mText;
                }
                if (line.mValue.size() > 1)
                {
                    line.mWrapped.assign(1, joined); // permissions stay on one line
                }
                else
                {
                    wrap(font, joined, value_width - (line.mSwatch.mV[VALPHA] > 0.f ? 14 : 0), line.mWrapped);
                }
                line.mHeight = llmax(1, (S32)line.mWrapped.size()) * line_height + ROW_GAP;
                break;
            }
        }
        top += line.mHeight;
    }
    mContentHeight = top + PAD;
    mScrollbar->setRect(LLRect(width - LLScrollbar::THIN_THICKNESS, getRect().getHeight(), width, 0));
    mScrollbar->setDocSize(mContentHeight);
    mScrollbar->setPageSize(getRect().getHeight());
    const bool scrolls = mContentHeight > getRect().getHeight();
    const bool changed = scrolls != mScrollbar->getVisible();
    mScrollbar->setVisible(scrolls);
    mScrollbar->setDocPos(mScrollbar->getDocPos());
    if (changed)
    {
        mLaidWidth = -1;    // laid out again next frame in the new width
    }
}

void SKInventoryDetailsView::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLUICtrl::reshape(width, height, called_from_parent);
    mLaidWidth = -1;
}

S32 SKInventoryDetailsView::contentTop() const
{
    return getRect().getHeight() + mScrollbar->getDocPos();
}

S32 SKInventoryDetailsView::lineAt(S32 y) const
{
    const S32 down = contentTop() - y;
    for (S32 i = 0; i < (S32)mLines.size(); ++i)
    {
        if (down >= mLines[i].mTop && down < mLines[i].mTop + mLines[i].mHeight)
        {
            return i;
        }
    }
    return -1;
}

void SKInventoryDetailsView::draw()
{
    static LLUIColor text_color = LLUIColorTable::instance().getColor("LabelTextColor");
    static LLUIColor dim_color = LLUIColorTable::instance().getColor("LtGray_50");
    static LLUIColor link_color = LLUIColorTable::instance().getColor("HTMLLinkColor");
    static LLUIColor rule_color = LLUIColorTable::instance().getColor("White_10");

    if (!mFetching && mItemID.notNull() && mSettle.getElapsedTimeF32() >= SETTLE_SECONDS)
    {
        mFetching = true;
        mDirty = true;
    }
    if (mDirty || mPending || mRevision != SKInventoryAssets::revision())
    {
        rebuild();
    }
    if (mLaidWidth != getRect().getWidth())
    {
        layout();
    }

    mPlayer.hide();
    mThumb.hide();
    const S32 height = getRect().getHeight();
    const S32 width = getRect().getWidth();
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifBold();
    if (!mMessage.empty())
    {
        font->renderUTF8(mMessage, 0, (S32)(width / 2), (S32)(height / 2), dim_color.get(), LLFontGL::HCENTER, LLFontGL::VCENTER);
        LLUICtrl::draw();
        return;
    }

    {
        LLLocalClipRect clip(getLocalRect());
        const S32 top = contentTop();
        const S32 inner_left = PAD;
        const S32 inner_right = width - PAD - (mScrollbar->getVisible() ? LLScrollbar::THIN_THICKNESS : 0);
        if (mMediaHeight > 0)
        {
            const LLRect media(inner_left, top - PAD, inner_right, top - PAD - mMediaHeight);
            ((mThumbnail && !mThumbBelow) ? mThumb : mPlayer).draw(media, mHoverMedia);
        }
        const S32 line_height = (S32)font->getLineHeight();
        for (S32 i = 0; i < (S32)mLines.size(); ++i)
        {
            const Line& line = mLines[i];
            const S32 y = top - line.mTop;
            if (y - line.mHeight > height || y < 0)
            {
                continue;
            }
            switch (line.mKind)
            {
                case Line::LINE_NAME:
                {
                    S32 ly = y - ROW_GAP;
                    for (const std::string& part : line.mWrapped)
                    {
                        bold->renderUTF8(part, 0, (S32)inner_left, (S32)ly, text_color.get(), LLFontGL::LEFT, LLFontGL::TOP);
                        ly -= (S32)bold->getLineHeight();
                    }
                    break;
                }
                case Line::LINE_HEADING:
                    gl_line_2d(inner_left, y - 4, inner_right, y - 4, rule_color.get());
                    font->renderUTF8(line.mLabel, 0, (S32)inner_left, (S32)(y - 7), dim_color.get(), LLFontGL::LEFT, LLFontGL::TOP);
                    break;
                case Line::LINE_PLAYER:
                {
                    // The player under the rows when the thumbnail took the top, or a texture's thumbnail under it.
                    const LLRect rect(inner_left, y, inner_right, y - line.mHeight + PAD);
                    (mThumbBelow ? mThumb : mPlayer).draw(rect, mHoverLine == i);
                    break;
                }
                default:
                {
                    font->renderUTF8(line.mLabel, 0, (S32)inner_left, (S32)y, dim_color.get(), LLFontGL::LEFT, LLFontGL::TOP);
                    S32 x = inner_left + LABEL_WIDTH;
                    if (line.mSwatch.mV[VALPHA] > 0.f)
                    {
                        gl_rect_2d(x, y - 2, x + 10, y - 12, line.mSwatch, true);
                        x += 14;
                    }
                    const bool link = !line.mURL.empty();
                    const LLColor4 color = link ? link_color.get() : text_color.get();
                    if (line.mValue.size() > 1)
                    {
                        for (const Segment& segment : line.mValue)
                        {
                            const S32 w = font->getWidth(segment.mText);
                            const LLColor4 seg_color = segment.mStruck ? dim_color.get() : color;
                            font->renderUTF8(segment.mText, 0, (S32)x, (S32)y, seg_color, LLFontGL::LEFT, LLFontGL::TOP);
                            if (segment.mStruck)
                            {
                                gl_line_2d(x, y - line_height / 2, x + w, y - line_height / 2, seg_color);
                            }
                            x += w + font->getWidth(std::string(" "));
                        }
                    }
                    else
                    {
                        S32 ly = y;
                        for (const std::string& part : line.mWrapped)
                        {
                            font->renderUTF8(part, 0, (S32)x, (S32)ly, color, LLFontGL::LEFT, LLFontGL::TOP, link && mHoverLine == i ? LLFontGL::UNDERLINE : LLFontGL::NORMAL);
                            ly -= line_height;
                        }
                    }
                    break;
                }
            }
        }
    }
    LLUICtrl::draw();
}

bool SKInventoryDetailsView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (childrenHandleMouseDown(x, y, mask))
    {
        return true;
    }
    if (mPlayer.handleMouseDown(x, y) || mThumb.handleMouseDown(x, y))
    {
        return true;
    }
    const S32 index = lineAt(y);
    if (index >= 0 && !mLines[index].mURL.empty() && x >= PAD + LABEL_WIDTH)
    {
        make_ui_sound("UISndClick");
        if (mLines[index].mURL == "aspect:")
        {
            const S32 line_top = contentTop() - mLines[index].mTop;
            const LLRect rect(PAD + LABEL_WIDTH, line_top, getRect().getWidth() - PAD - (mScrollbar->getVisible() ? LLScrollbar::THIN_THICKNESS : 0),
                              line_top - mLines[index].mHeight);
            SKInventoryDetails::showAspectMenu(this, rect, target_item(mItemID) ? target_item(mItemID)->getUUID() : mItemID, false);
        }
        else
        {
            LLUrlAction::clickAction(mLines[index].mURL, true);
        }
        return true;
    }
    return LLUICtrl::handleMouseDown(x, y, mask);
}

bool SKInventoryDetailsView::handleHover(S32 x, S32 y, MASK mask)
{
    mHoverLine = lineAt(y);
    const S32 top = contentTop();
    mHoverMedia = mMediaHeight > 0 && y <= top - PAD && y >= top - PAD - mMediaHeight;
    mPlayer.handleHover(x, y);
    if (mHoverLine >= 0 && !mLines[mHoverLine].mURL.empty() && x >= PAD + LABEL_WIDTH)
    {
        gViewerWindow->getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    return LLUICtrl::handleHover(x, y, mask);
}

void SKInventoryDetailsView::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHoverLine = -1;
    mHoverMedia = false;
    mPlayer.mouseLeave();
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool SKInventoryDetailsView::handleScrollWheel(S32 x, S32 y, LLScrollDelta delta)
{
    if (mContentHeight <= getRect().getHeight())
    {
        return false;
    }
    mScrollbar->setDocPos(mScrollbar->getDocPos() + delta.mClicks * WHEEL_PIXELS);
    return true;
}

bool SKInventoryDetailsView::handleToolTip(S32 x, S32 y, MASK mask)
{
    std::string tip;
    if (mPlayer.getToolTip(x, y, tip))
    {
        LLToolTipMgr::instance().show(tip);
        return true;
    }
    const S32 index = lineAt(y);
    if (index >= 0 && !mLines[index].mToolTip.empty())
    {
        LLToolTipMgr::instance().show(mLines[index].mToolTip);
        return true;
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}
