/**
 * @file skinventorydetails.h
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

#ifndef SK_SKINVENTORYDETAILS_H
#define SK_SKINVENTORYDETAILS_H

#include "lluictrl.h"
#include "llframetimer.h"
#include "skinventoryflatview.h"

#include <map>

class LLFetchedGLTFMaterial;
class LLScrollbar;
class LLViewerFetchedTexture;
class LLViewerInventoryItem;
class LLViewerTexture;

// Strings, the shared texture aspect writer (§9.6) and its menu.
namespace SKInventoryDetails
{
    // Handed in by the floater, so every string stays in XUI.
    void setStrings(const std::map<std::string, std::string>& strings, const SKInventoryListLabels& labels);
    const std::string& str(const std::string& key);
    const SKInventoryListLabels& labels();

    // A strict trailing "(w:h)", one or two digits each; false (and 1:1) for none.
    bool aspectOf(const std::string& desc, S32& w, S32& h);
    // A texture's own width:height, reduced, once its size is known; false (and 1:1) before.
    bool naturalAspect(const LLUUID& image_id, S32& w, S32& h);
    // A texture's aspect: its description's tail, else its own width:height, else 1:1.
    void aspectFor(const LLInventoryItem* item, S32& w, S32& h);
    // Modify permission, outside the Library; the tooltip says why not.
    bool canSetAspect(const LLUUID& item_id, std::string* why = nullptr);
    void setAspect(const LLUUID& item_id, S32 w, S32 h);
    // The nine shapes, the current one checked; opens above the rect when asked, else below.
    void showAspectMenu(LLView* spawn, const LLRect& rect, const LLUUID& item_id, bool above);
    void hideAspectMenu();
}

// Previews one item in a rect: an image, a sound's waveform, an animation's timeline, a material's sphere or a
// landmark's map. Plays through the same calls the preview floaters make, without them.
class SKInventoryPlayer
{
public:
    enum EKind
    {
        KIND_NONE,
        KIND_IMAGE,
        KIND_SOUND,
        KIND_ANIMATION,
        KIND_MATERIAL,
        KIND_LANDMARK,
    };

    SKInventoryPlayer();
    ~SKInventoryPlayer();

    // The item (links resolved); image_id draws that picture instead of a player (a thumbnail).
    void setItem(const LLUUID& item_id, const LLUUID& image_id = LLUUID::null);
    // Assets are fetched only once the selection has settled.
    void setFetching(bool fetching) { mFetching = fetching; }
    // Gallery: the wheel zooms the image, and it is drawn larger.
    void setZoomable(bool zoomable) { mZoomable = zoomable; }

    EKind getKind() const { return mKind; }
    const LLUUID& getAssetID() const { return mAssetID; }
    // The height it wants at a width; 0 for nothing to show.
    S32 height(S32 width) const;
    // The drawn image's rect, for chips placed beside it.
    const LLRect& getImageRect() const { return mImageRect; }
    LLViewerFetchedTexture* getTexture() const { return mTexture; }

    void draw(const LLRect& rect, bool hovered);
    bool handleMouseDown(S32 x, S32 y);
    bool handleDoubleClick(S32 x, S32 y);
    void handleHover(S32 x, S32 y);
    bool handleScrollWheel(S32 x, S32 y, S32 clicks);
    bool getToolTip(S32 x, S32 y, std::string& tip) const;
    void mouseLeave();
    // Not drawn this frame: takes no mouse.
    void hide() { mRect = LLRect(); mImageRect = LLRect(); }
    // Stops whatever plays: on a new item, or when the view goes.
    void stop();
    bool isZoomed() const { return mZoom > 1.f; }

private:
    void drawImage(const LLRect& rect);
    void drawSound(const LLRect& rect);
    void drawAnimation(const LLRect& rect);
    void drawMaterial(const LLRect& rect);
    void drawLandmark(const LLRect& rect);
    void drawButtons(const LLRect& rect);
    void drawMessage(const LLRect& rect, const std::string& text);
    LLRect waveRect() const;
    void pressButton(S32 index);
    F32 animTime(F64 now, F32& weight);

    EKind mKind;
    LLUUID mItemID;
    LLUUID mAssetID;
    LLUUID mImageID;
    F32 mAspect;
    bool mFetching;
    bool mZoomable;
    LLRect mRect;
    LLRect mImageRect;
    LLRect mButtons[2];
    S32 mHoverButton;
    S32 mHoverX;

    LLPointer<LLViewerFetchedTexture> mTexture;
    LLPointer<LLFetchedGLTFMaterial> mMaterial;
    LLPointer<LLViewerTexture> mMaterialPreview;

    // Sound: a marked time, and the playback clock.
    F32 mMark;
    bool mSoundPlaying;
    bool mSoundLocal = false;   // a local preview, else our own copy of an inworld trigger
    bool mSoundHeard = false;   // the local preview has reported a position since it started
    bool mWaveDrag = false;     // the mouse went down on the waveform and is held
    F32 mSoundStart = 0.f;      // where the local preview started, for the clock where the engine reports none
    LLUUID mSoundSource;        // the local preview's audio source
    LLFrameTimer mSoundTimer;

    void playLocal(F32 at);
    void seekWave(S32 x);

    // Animation: which button plays it, when it started, and when it was stopped.
    S32 mAnimMode;  // -1 none, 0 locally, 1 inworld
    F64 mAnimStart;
    F64 mAnimStop;

    // Gallery zoom: eased to its target; the pointer's place over the stage picks the part shown.
    F32 mZoom;
    F32 mZoomShown;
    LLVector2 mZoomOrigin;
    LLFrameTimer mZoomTimer;
};

// The details pane, and the details column in Columns: the rows about the selection and the player.
class SKInventoryDetailsView : public LLUICtrl
{
public:
    AL_VIEW_TYPE(SKInventoryDetailsView, LLUICtrl);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
    };

    ~SKInventoryDetailsView();

    // A single item shows; several, none or a folder say so.
    void setSelection(const uuid_vec_t& ids, const std::string& folder_path = std::string());
    const LLUUID& getItemID() const { return mItemID; }
    // Reads the item again: its name, description or permissions changed.
    void refresh() { mDirty = true; }

    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void onVisibilityChange(bool new_visibility) override;

protected:
    SKInventoryDetailsView(const Params& p);
    friend class LLUICtrlFactory;

    struct Segment
    {
        std::string mText;
        bool mDim = false;
        bool mStruck = false;
    };

    struct Line
    {
        enum EKind
        {
            LINE_NAME,
            LINE_HEADING,
            LINE_ROW,
            LINE_MESSAGE,
            LINE_PLAYER,
        };
        EKind mKind = LINE_ROW;
        std::string mLabel;
        std::vector<Segment> mValue;
        std::string mURL;       // a link: a profile, a teleport, or "aspect:" for the aspect menu
        std::string mToolTip;
        LLColor4 mSwatch = LLColor4::transparent;
        // Laid out: the value's wrapped lines, and where the line sits in content pixels from the top.
        std::vector<std::string> mWrapped;
        S32 mTop = 0;
        S32 mHeight = 0;
    };

    void rebuild();
    void layout();
    void addRow(const std::string& label_key, const std::string& value, const std::string& url = std::string(),
                const std::string& tip = std::string());
    void addHeading(const std::string& key);
    void addTypeRows(LLViewerInventoryItem* item, LLViewerInventoryItem* target);
    void addPermsRow(const std::string& label_key, U32 mask);
    S32 lineAt(S32 y) const;
    S32 contentTop() const;

    uuid_vec_t mSelection;
    LLUUID mItemID;
    std::string mFolderPath;
    std::string mMessage;
    bool mThumbnail;
    bool mThumbBelow = false;   // a texture or photo with a thumbnail: the texture on top, the thumbnail below the rows
    std::vector<Line> mLines;
    S32 mMediaHeight;   // the picture or player at the top
    S32 mContentHeight;
    bool mDirty;
    bool mPending;      // something still loads: rows are read again each frame until it lands
    U32 mRevision;
    S32 mLaidWidth;
    S32 mHoverLine;
    bool mHoverMedia;
    LLFrameTimer mSettle;
    bool mFetching;
    SKInventoryPlayer mPlayer;
    SKInventoryPlayer mThumb;      // the item's thumbnail, on top of the rows
    LLPointer<LLViewerFetchedTexture> mInfoTexture;    // a texture's own image, for its resolution
    LLScrollbar* mScrollbar;
    boost::signals2::connection mNameConnection;
};

#endif // SK_SKINVENTORYDETAILS_H
