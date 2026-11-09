/**
 * @file sklegendview.h
 * @brief A legend for an overlay, a debug view or a chart, laid out into whatever rect XUI gives it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

#ifndef SK_LEGENDVIEW_H
#define SK_LEGENDVIEW_H

#include "alviewtype.h"
#include "lluictrl.h"
#include "sklegendmodel.h"
#include "skticks.h"

#include <boost/signals2.hpp>

class LLFontGL;

// Draws every frame from a layout cached on setLegend and reshape; never resizes itself.
class SKLegendView : public LLUICtrl
{
public:
    AL_VIEW_TYPE(SKLegendView, LLUICtrl);

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<bool> hud;    // the in-viewport ground, thinner than a floater's
        Optional<bool> framed; // ground and frame behind the content
        Params();
    };

    // One laid-out line, in local coordinates.
    struct Line
    {
        enum class Kind : U8
        {
            Title,
            Subtitle,
            Heading,
            Format,
            RampBar,
            RampTicks,
            Row,
            Note,
            More,
            TooSmall
        };
        Kind   kind    = Kind::Row;
        S32    section = -1;
        S32    row     = -1;
        LLRect rect;
    };

    void            setLegend(const SKLegend& legend);
    const SKLegend& legend() const { return mLegend; }

    // From outside: the world or a chart is hovered.
    void setHighlight(const SKVizKey& key);
    void clearHighlight() { setHighlight(SKVizKey()); }
    const SKVizKey& highlight() const { return mHighlight; }
    const SKVizKey& hovered() const { return mHover; }
    // A live reading on the ramps of one scale, or all of them for "", without relayout; NaN clears.
    void setMark(F64 value, const std::string& scale = std::string());

    using hover_signal_t = boost::signals2::signal<void(const SKVizKey&)>;
    boost::signals2::connection onHoverKey(const hover_signal_t::slot_type& slot) { return mHoverSignal.connect(slot); }

    // What the legend needs laid out whole, for a parent sizing it in XUI terms and for tests.
    S32 contentHeight() const { return mContentHeight; }
    const std::vector<Line>& lines() const { return mLines; }
    S32  hiddenRows() const { return mHiddenRows; }
    bool tooSmall() const { return mTooSmall; }
    // The ramp bar's ticks for a section, as laid out.
    const SKTicks* ticksOf(S32 section) const;
    // Where a value sits along a section's ramp in [0, 1] as drawn, NaN where it is off the bar.
    F64 rampPosition(S32 section, F64 value) const;
    // Where setMark's reading sits on a section's ramp, NaN for none.
    F64 markPosition(S32 section) const;
    // What a point in local coordinates stands for, empty for nothing.
    SKVizKey keyAt(S32 x, S32 y) const;
    // Whether a row is drawn lit, by the hover or the highlight.
    bool isRowLit(S32 section, S32 row) const;

    // Lays out again if a ramp's scale moved since the last layout; draw calls it every frame.
    void refreshIfScalesChanged();

    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

protected:
    friend class LLUICtrlFactory;
    SKLegendView(const Params& p);

private:
    void layout();
    void drawRamp(const Line& line, const SKLegendSection& sec, F32 alpha) const;
    void drawRow(const Line& line, const SKLegendSection& sec, const SKLegendRow& row, F32 alpha) const;
    S32  marksWidth(const std::vector<SKLegendMark>& marks) const;
    S32  rowHeight() const;
    U32  scalesRevision() const;

    SKLegend          mLegend;
    SKVizKey          mHover;
    SKVizKey          mHighlight;
    std::string       mMarkScale;
    F64               mMark;
    bool              mHud;
    bool              mFramed;
    const LLFontGL*   mLabelFont;
    const LLFontGL*   mValueFont;
    std::vector<Line> mLines;
    std::vector<SKTicks> mTicks;      // per section
    std::vector<S32>  mMarkColumn;     // per section, the marks' width
    S32               mContentHeight = 0;
    S32               mHiddenRows    = 0;
    bool              mTooSmall      = false;
    U32               mLaidRevision  = 0;
    hover_signal_t    mHoverSignal;
};

#endif // SK_LEGENDVIEW_H
