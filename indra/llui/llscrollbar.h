/**
 * @file llscrollbar.h
 * @brief Scrollbar UI widget
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#ifndef LL_SCROLLBAR_H
#define LL_SCROLLBAR_H

#include "stdtypes.h"
#include "lluictrl.h"
#include "v4color.h"
#include "llbutton.h"
#include "skscrollanimator.h" // <SK:Nexii> smooth scroll

//
// Classes
//
class LLScrollbar final
: public LLUICtrl
{
public:
    AL_VIEW_TYPE(LLScrollbar, LLUICtrl);

    typedef std::function<void (S32, LLScrollbar*)> callback_t;
    struct Params
    :   public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Mandatory<EOrientation>         orientation;
        Mandatory<S32>                  doc_size;
        Mandatory<S32>                  doc_pos;
        Mandatory<S32>                  page_size;

        Optional<callback_t>            change_callback;
        Optional<S32>                   step_size;
        Optional<S32>                   thickness;

        Optional<LLUIImage*>            thumb_image_vertical,
                                        thumb_image_horizontal,
                                        track_image_horizontal,
                                        track_image_vertical;

        Optional<bool>                  bg_visible;
        Optional<bool>                  overlay; // <SK:Nexii> Overlay scroll bar: thin fading thumb over the content, no track or arrows

        Optional<LLUIColor>             track_color,
                                        thumb_color,
                                        bg_color;

        Optional<LLButton::Params>      up_button;
        Optional<LLButton::Params>      down_button;
        Optional<LLButton::Params>      left_button;
        Optional<LLButton::Params>      right_button;

        Params();
    };

protected:
    LLScrollbar (const Params & p);
    friend class LLUICtrlFactory;

public:
    virtual ~LLScrollbar();

    virtual void setValue(const LLSD& value) override;

    // Overrides from LLView
    virtual bool    handleKeyHere(KEY key, MASK mask) override;
    virtual bool    handleMouseDown(S32 x, S32 y, MASK mask) override;
    virtual bool    handleMouseUp(S32 x, S32 y, MASK mask) override;
    virtual bool    handleDoubleClick(S32 x, S32 y, MASK mask) override;
    virtual bool    handleHover(S32 x, S32 y, MASK mask) override;
    virtual bool    handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;
    virtual bool    handleScrollHWheel(S32 x, S32 y, LLScrollDelta delta) override;
    virtual bool    handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
        EDragAndDropType cargo_type, void *cargo_data, EAcceptance *accept, std::string &tooltip_msg) override;

    virtual void    reshape(S32 width, S32 height, bool called_from_parent = true) override;

    virtual void    draw() override;
    // <SK:Nexii> Overlay scroll bar
    virtual void    onVisibilityChange(bool new_visibility) override;
    bool                isOverlay() const { return mOverlay; }
    // </SK:Nexii>

    // How long the "document" is.
    void                setDocSize( S32 size );
    S32                 getDocSize() const      { return mDocSize; }

    // How many "lines" the "document" has scrolled.
    // 0 <= DocPos <= DocSize - DocVisibile
    bool                setDocPos( S32 pos, bool update_thumb = true );
    S32                 getDocPos() const       { return mDocPos; }

    bool                isAtBeginning() const;
    bool                isAtEnd() const;

    // Setting both at once.
    void                setDocParams( S32 size, S32 pos );

    // How many "lines" of the "document" is can appear on a page.
    void                setPageSize( S32 page_size );
    S32                 getPageSize() const     { return mPageSize; }

    // The farthest the document can be scrolled (top of the last page).
    S32                 getDocPosMax() const    { return llmax( 0, mDocSize - mPageSize); }

    void                pageUp(S32 overlap);
    void                pageDown(S32 overlap);

    void                onLineUpBtnPressed(const LLSD& data);
    void                onLineDownBtnPressed(const LLSD& data);

    S32                 getThickness() const { return mThickness; }
    void                setThickness(S32 thickness);

    // <SK:Nexii> smooth scroll
    // The wheel glides the position; getDocPos() is where it rounds to, getDocPosF() where it is.
    F32                 getDocPosF() const { return mDocPosF; }
    // Pixels per doc unit (1 where the doc is in pixels, a row's height where it is in rows), and per wheel line.
    void                setWheelUnits(F32 unit_pixels, F32 line_pixels);
    // Advances the wheel's glide to now; its owner calls this every frame it draws.
    void                tickWheel();
    bool                isWheelGliding() const { return mWheelAnimator.isAnimating(); }
    bool                scrollByWheel(const LLScrollDelta& delta);
    // </SK:Nexii>

private:
    void                updateThumbRect();
    bool                changeLine(S32 delta, bool update_thumb );
    // <SK:Nexii> Overlay scroll bar
    S32                 arrowLength() const { return mOverlay ? 0 : mThickness; }
    F32                 overlayAlpha(F64 now) const;
    void                overlayTouch();
    void                drawOverlay();
    // </SK:Nexii>

    callback_t          mChangeCallback;

    const EOrientation  mOrientation;
    S32                 mDocSize;       // Size of the document that the scrollbar is modeling.  Units depend on the user.  0 <= mDocSize.
    S32                 mDocPos;        // Position within the doc that the scrollbar is modeling, in "lines" (user size)
    S32                 mPageSize;      // Maximum number of lines that can be seen at one time.
    S32                 mStepSize;
    // <SK:Nexii> smooth scroll
    SKScrollAnimator    mWheelAnimator;
    F32                 mDocPosF = 0.f;
    F32                 mWheelUnit = 1.f;
    F32                 mWheelLine = 1.f;
    bool                mApplyingWheel = false;
    // </SK:Nexii>
    bool                mDocChanged;

    LLRect              mThumbRect;
    S32                 mDragStartX;
    S32                 mDragStartY;
    F32                 mHoverGlowStrength;
    F32                 mCurGlowStrength;

    LLRect              mOrigRect;
    S32                 mLastDelta;

    LLUIColor           mTrackColor;
    LLUIColor           mThumbColor;
    LLUIColor           mBGColor;

    bool                mBGVisible;

    LLUIImagePtr        mThumbImageV;
    LLUIImagePtr        mThumbImageH;
    LLUIImagePtr        mTrackImageV;
    LLUIImagePtr        mTrackImageH;

    S32                 mThickness;

    LLButton*           mLineUpBtn = nullptr;
    LLButton*           mLineDownBtn = nullptr;

    // <SK:Nexii> Overlay scroll bar
    bool                mOverlay = false;
    bool                mOverlayRevealPending = false;
    F32                 mOverlayWiden = 0.f;
    F64                 mOverlayShownAt = -1000.0;
    F64                 mOverlayActiveAt = -1000.0;
    U32                 mOverlayDrawnFrame = 0;
    // </SK:Nexii>
};


#endif  // LL_SCROLLBAR_H
