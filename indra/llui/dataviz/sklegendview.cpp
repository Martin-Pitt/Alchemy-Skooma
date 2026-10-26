/**
 * @file sklegendview.cpp
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

#include "linden_common.h"

#include "sklegendview.h"

#include "alsurface.h"
#include "llfontgl.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "skdatavizstyle.h"

#include <algorithm>
#include <cmath>

namespace
{
    using namespace SKDataVizStyle;
    using Line = SKLegendView::Line;

    constexpr S32 MARK_SPACE = 3; // between marks of one row

    LLColor4 faded(const LLColor4& c, F32 alpha) { return LLColor4(c.mV[0], c.mV[1], c.mV[2], c.mV[3] * alpha); }

    S32 barWidth(const LLRect& line) { return std::min(line.getWidth(), RAMP_W); }

    LLColor4 rampColour(const SKColorScale& s, F32 t)
    {
        if (s.isDiscrete())
        {
            const S32 n = (S32)s.stops().size();
            return s.classColour(std::min(n - 1, (S32)(t * (F32)n)));
        }
        if (s.kind() == SKColorScale::Kind::Identity)
        {
            return LLColor4(t, t, t, 1.f);
        }
        return s.at(t);
    }

    std::string headingOf(const SKLegendSection& sec)
    {
        std::string h = sec.heading.empty() ? sec.quantity : sec.heading;
        if (sec.type == SKLegendSection::Type::Ramp && !sec.unit.empty())
        {
            h += h.empty() ? sec.unit : " (" + sec.unit + ")";
        }
        return h;
    }

    LLColor4 markerColour(SKLegendMarker::Kind k)
    {
        switch (k)
        {
            case SKLegendMarker::Kind::Selected:
                return selected();
            case SKLegendMarker::Kind::Reference:
                return reference();
            default:
                return threshold();
        }
    }
}

SKLegendView::Params::Params() : hud("hud", false), framed("framed", true)
{
    mouse_opaque = false;
}

SKLegendView::SKLegendView(const Params& p)
    : LLUICtrl(p),
      mMark(std::nan("")),
      mHud(p.hud),
      mFramed(p.framed),
      mLabelFont(LLFontGL::getFontSansSerifSmall()),
      mValueFont(LLFontGL::getFontMonospace())
{
    layout();
}

void SKLegendView::setLegend(const SKLegend& legend)
{
    mLegend = legend;
    mHover  = SKVizKey();
    layout();
}

void SKLegendView::setHighlight(const SKVizKey& key) { mHighlight = key; }

void SKLegendView::setMark(F64 value, const std::string& scale)
{
    mMark      = value;
    mMarkScale = scale;
}

void SKLegendView::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLUICtrl::reshape(width, height, called_from_parent);
    layout();
}

S32 SKLegendView::rowHeight() const
{
    S32 lh = 12;
    if (mLabelFont && mValueFont)
    {
        lh = std::max(mLabelFont->getLineHeight(), mValueFont->getLineHeight());
    }
    return lh + 2;
}

U32 SKLegendView::scalesRevision() const
{
    U32 r = 0;
    for (const SKLegendSection& sec : mLegend.sections)
    {
        if (sec.scale)
        {
            r = r * 31u + sec.scale->revision() + 1u;
        }
    }
    return r;
}

S32 SKLegendView::marksWidth(const std::vector<SKLegendMark>& marks) const
{
    S32 w = 0;
    for (const SKLegendMark& m : marks)
    {
        S32 mw = SWATCH;
        switch (m.kind)
        {
            case SKLegendMark::Kind::Line:
            case SKLegendMark::Kind::Dashed:
            case SKLegendMark::Kind::Arrow:
                mw = LINE_MARK;
                break;
            case SKLegendMark::Kind::Strip:
                mw = std::max<S32>(1, (S32)m.strip.size()) * 4;
                break;
            case SKLegendMark::Kind::Text:
                mw = mValueFont ? mValueFont->getWidth(m.token) : 6 * (S32)m.token.size();
                break;
            default:
                break;
        }
        w += (w ? MARK_SPACE : 0) + mw;
    }
    return w;
}

void SKLegendView::layout()
{
    mLaidRevision = scalesRevision();
    mLines.clear();
    mTicks.assign(mLegend.sections.size(), SKTicks());
    mMarkColumn.assign(mLegend.sections.size(), 0);
    mHiddenRows = 0;
    mTooSmall   = false;

    const S32 W     = getRect().getWidth();
    const S32 H     = getRect().getHeight();
    const S32 row_h = rowHeight();
    const S32 gap   = (row_h - 2) / 2;
    const S32 avail = H - 2 * PAD;

    // Every line with the gap above it, before fitting.
    struct Want
    {
        Line l;
        S32  h;
        S32  above;
    };
    std::vector<Want> want;
    auto add = [&](Line::Kind k, S32 sec, S32 row, S32 h, S32 above) { want.push_back({ { k, sec, row, LLRect() }, h, above }); };
    if (!mLegend.title.empty())
    {
        add(Line::Kind::Title, -1, -1, row_h, 0);
    }
    if (!mLegend.subtitle.empty())
    {
        add(Line::Kind::Subtitle, -1, -1, row_h, 0);
    }
    for (S32 s = 0; s < (S32)mLegend.sections.size(); ++s)
    {
        const SKLegendSection& sec   = mLegend.sections[s];
        S32                    above = want.empty() ? 0 : gap;
        if (!headingOf(sec).empty())
        {
            add(Line::Kind::Heading, s, -1, row_h, above);
            above = 0;
        }
        if (!sec.format.empty())
        {
            add(Line::Kind::Format, s, -1, row_h, above);
            above = 0;
        }
        if (sec.type == SKLegendSection::Type::Ramp && sec.scale)
        {
            const bool ring = sec.shape == SKLegendSection::Shape::Ring;
            add(Line::Kind::RampBar, s, -1, ring ? RING_D : CARET + RAMP_H, above);
            above = 0;
            if (!ring)
            {
                add(Line::Kind::RampTicks, s, -1, TICK + row_h, 0);
            }
        }
        S32 col = 0;
        for (S32 r = 0; r < (S32)sec.rows.size(); ++r)
        {
            add(Line::Kind::Row, s, r, row_h, above);
            above = 0;
            col   = std::max(col, marksWidth(sec.rows[r].marks));
        }
        mMarkColumn[s] = col;
    }
    if (!mLegend.note.empty())
    {
        add(Line::Kind::Note, -1, -1, row_h, want.empty() ? 0 : gap);
    }

    S32 total = 0;
    for (const Want& w : want)
    {
        total += w.above + w.h;
    }
    mContentHeight = total + 2 * PAD;

    const bool has_title = !mLegend.title.empty();
    std::vector<Want> kept;
    S32 pad = PAD;
    if (W < MIN_WIDTH || (has_title ? 2 : 1) * row_h > avail)
    {
        // Collapsed: the title, else the words, at the least padding that still reads.
        mTooSmall       = true;
        pad             = MIN_PAD;
        const S32 small = H - 2 * pad;
        if (has_title && row_h <= small)
        {
            kept.push_back(want.front());
        }
        if ((S32)kept.size() * row_h + row_h <= small)
        {
            kept.push_back({ { Line::Kind::TooSmall, -1, -1, LLRect() }, row_h, 0 });
        }
    }
    else if (total <= avail)
    {
        kept = want;
    }
    else
    {
        S32    used = 0;
        size_t i    = 0;
        for (; i < want.size(); ++i)
        {
            if (used + want[i].above + want[i].h + row_h > avail)
            {
                break;
            }
            used += want[i].above + want[i].h;
            kept.push_back(want[i]);
        }
        for (; i < want.size(); ++i)
        {
            mHiddenRows += (want[i].l.kind == Line::Kind::Row || want[i].l.kind == Line::Kind::RampBar) ? 1 : 0;
        }
        // A ramp's ticks never stand without their bar, nor a bar without its ticks.
        if (!kept.empty() && kept.back().l.kind == Line::Kind::RampBar)
        {
            kept.pop_back();
            ++mHiddenRows;
        }
        while (!kept.empty() && (kept.back().l.kind == Line::Kind::Heading || kept.back().l.kind == Line::Kind::Format))
        {
            kept.pop_back();
        }
        if (mHiddenRows > 0)
        {
            kept.push_back({ { Line::Kind::More, -1, -1, LLRect() }, row_h, 0 });
        }
    }

    S32 top = H - pad;
    for (Want& w : kept)
    {
        top -= w.above;
        w.l.rect = LLRect(pad, top, pad + std::max(0, W - 2 * pad), top - w.h);
        top -= w.h;
        mLines.push_back(w.l);
    }

    // Ticks for the ramps laid out.
    const S32 char_px = mValueFont ? mValueFont->getWidth("0") : 6;
    for (const Line& l : mLines)
    {
        if (l.kind != Line::Kind::RampBar)
        {
            continue;
        }
        const SKColorScale& sc = *mLegend.sections[l.section].scale;
        SKTicks&            t  = mTicks[l.section];
        if (sc.kind() == SKColorScale::Kind::Binned)
        {
            for (F64 e : sc.edges())
            {
                t.values.push_back(e);
                t.labels.push_back(SKTicks::format(e, std::fabs(e - std::round(e)) > 1e-9 ? 1 : 0));
            }
        }
        else if (sc.transform() == SKColorScale::Transform::Log)
        {
            t = SKTicks::decades(sc.lo(), sc.hi());
        }
        else if (!sc.isDiscrete())
        {
            const S32 bw = barWidth(l.rect);
            // A glyph's advance can undercount what the font draws, so widen until the drawn labels clear.
            S32 cpx = char_px;
            for (S32 tries = 0; tries < 16; ++tries, ++cpx)
            {
                t = SKTicks::make(sc.lo(), sc.hi(), bw, cpx);
                bool fits = true;
                for (size_t i = 0; i + 1 < t.values.size() && mValueFont; ++i)
                {
                    const F64 gap = (t.values[i + 1] - t.values[i]) / (sc.hi() - sc.lo()) * bw;
                    fits = fits && gap >= (mValueFont->getWidth(t.labels[i]) + mValueFont->getWidth(t.labels[i + 1])) / 2 + 4;
                }
                if (fits)
                {
                    break;
                }
            }
            // The ends are always labelled; an inner tick whose label would touch one goes.
            const F64 range = sc.hi() - sc.lo();
            auto      clear = [&](F64 v, F64 end, const std::string& a, const std::string& b) {
                const S32 wa = mValueFont ? mValueFont->getWidth(a) : (S32)a.size() * cpx;
                const S32 wb = mValueFont ? mValueFont->getWidth(b) : (S32)b.size() * cpx;
                return std::fabs(v - end) / range * bw >= (F64)(wa + wb + 4);
            };
            SKTicks ends;
            ends.step     = t.step;
            ends.decimals = t.decimals;
            const std::string lo_label = SKTicks::format(sc.lo(), t.decimals), hi_label = SKTicks::format(sc.hi(), t.decimals);
            ends.values.push_back(sc.lo());
            ends.labels.push_back(lo_label);
            for (size_t i = 0; i < t.values.size(); ++i)
            {
                if (clear(t.values[i], sc.lo(), t.labels[i], lo_label) && clear(t.values[i], sc.hi(), t.labels[i], hi_label))
                {
                    ends.values.push_back(t.values[i]);
                    ends.labels.push_back(t.labels[i]);
                }
            }
            ends.values.push_back(sc.hi());
            ends.labels.push_back(hi_label);
            // Only as many decimals as the ticks that stayed need.
            S32 d = 0;
            for (; d < ends.decimals; ++d)
            {
                const F64 scale = std::pow(10.0, d);
                if (std::all_of(ends.values.begin(), ends.values.end(),
                                [&](F64 v) { return std::fabs(std::round(v * scale) / scale - v) <= 1e-9 * std::max(1.0, std::fabs(v)); }))
                {
                    break;
                }
            }
            ends.decimals = d;
            for (size_t i = 0; i < ends.values.size(); ++i)
            {
                ends.labels[i] = SKTicks::format(ends.values[i], d);
            }
            t = std::move(ends);
        }
    }
}

F64 SKLegendView::rampPosition(S32 section, F64 value) const
{
    if (section < 0 || section >= (S32)mLegend.sections.size() || !mLegend.sections[section].scale || std::isnan(value))
    {
        return std::nan("");
    }
    const SKColorScale& sc = *mLegend.sections[section].scale;
    F64                 u  = sc.unitOf(value);
    if (sc.kind() == SKColorScale::Kind::Binned)
    {
        // Classes are drawn equal width, so a value sits within its class's share.
        const S32 i = sc.classOf(value);
        if (i < 0 || i + 1 >= (S32)sc.edges().size())
        {
            return std::nan("");
        }
        const F64 e0 = sc.edges()[i], e1 = sc.edges()[i + 1];
        u            = ((F64)i + (e1 > e0 ? (value - e0) / (e1 - e0) : 0.0)) / (F64)sc.stops().size();
    }
    return u >= 0.0 && u <= 1.0 ? u : std::nan("");
}

F64 SKLegendView::markPosition(S32 section) const
{
    if (section < 0 || section >= (S32)mLegend.sections.size() || !mLegend.sections[section].scale ||
        !(mMarkScale.empty() || mMarkScale == mLegend.sections[section].scale->id()))
    {
        return std::nan("");
    }
    return rampPosition(section, mMark);
}

const SKTicks* SKLegendView::ticksOf(S32 section) const
{
    return section >= 0 && section < (S32)mTicks.size() ? &mTicks[section] : nullptr;
}

SKVizKey SKLegendView::keyAt(S32 x, S32 y) const
{
    for (const Line& l : mLines)
    {
        if (!l.rect.pointInRect(x, y))
        {
            continue;
        }
        if (l.kind == Line::Kind::Row)
        {
            return mLegend.sections[l.section].rows[l.row].key;
        }
        if (l.kind == Line::Kind::RampBar)
        {
            const SKColorScale& sc = *mLegend.sections[l.section].scale;
            const S32           bw = barWidth(l.rect);
            if (x >= l.rect.mLeft + bw)
            {
                return SKVizKey();
            }
            const F64 t = ((F64)(x - l.rect.mLeft) + 0.5) / (F64)bw;
            SKVizKey  k;
            k.scale = sc.id();
            if (sc.isDiscrete())
            {
                k.category = (U32)std::min((S32)sc.stops().size() - 1, (S32)(t * (F64)sc.stops().size()));
            }
            else
            {
                k.value = sc.valueOf(t);
            }
            return k;
        }
    }
    return SKVizKey();
}

bool SKLegendView::isRowLit(S32 section, S32 row) const
{
    if (section < 0 || section >= (S32)mLegend.sections.size() || row < 0 || row >= (S32)mLegend.sections[section].rows.size())
    {
        return false;
    }
    const SKVizKey& k = mLegend.sections[section].rows[row].key;
    return !k.empty() && (k.matchesRow(mHover) || k.matchesRow(mHighlight));
}

bool SKLegendView::handleHover(S32 x, S32 y, MASK mask)
{
    const SKVizKey k = keyAt(x, y);
    if (k != mHover)
    {
        mHover = k;
        mHoverSignal(mHover);
    }
    return LLUICtrl::handleHover(x, y, mask);
}

void SKLegendView::onMouseLeave(S32 x, S32 y, MASK mask)
{
    if (!mHover.empty())
    {
        mHover = SKVizKey();
        mHoverSignal(mHover);
    }
    LLUICtrl::onMouseLeave(x, y, mask);
}

void SKLegendView::drawRamp(const Line& l, const SKLegendSection& sec, F32 alpha) const
{
    const SKColorScale& sc  = *sec.scale;
    const S32           bw  = barWidth(l.rect);
    const S32           x0  = l.rect.mLeft;
    const S32           bot = l.rect.mBottom;
    const LLColor4      ink = text();

    gGL.getTextureSlot(0)->unbind();
    if (sec.shape == SKLegendSection::Shape::Ring)
    {
        const F32 cx = (F32)x0 + RING_D * 0.5f, cy = (F32)bot + RING_D * 0.5f;
        const F32 ro = RING_D * 0.5f - 1.f, ri = ro - RAMP_H;
        constexpr S32 STEPS = 96;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < STEPS; ++i)
        {
            const F32 a0 = F_TWO_PI * i / STEPS, a1 = F_TWO_PI * (i + 1) / STEPS;
            gGL.color4fv(faded(rampColour(sc, (i + 0.5f) / STEPS), alpha).mV);
            gGL.vertex2f(cx + ri * cosf(a0), cy + ri * sinf(a0));
            gGL.vertex2f(cx + ro * cosf(a0), cy + ro * sinf(a0));
            gGL.vertex2f(cx + ro * cosf(a1), cy + ro * sinf(a1));
            gGL.vertex2f(cx + ri * cosf(a0), cy + ri * sinf(a0));
            gGL.vertex2f(cx + ro * cosf(a1), cy + ro * sinf(a1));
            gGL.vertex2f(cx + ri * cosf(a1), cy + ri * sinf(a1));
        }
        gGL.end();
        return;
    }

    gGL.begin(LLRender::TRIANGLES);
    for (S32 i = 0; i < bw; ++i)
    {
        const F32 t = ((F32)i + 0.5f) / (F32)bw;
        switch (sec.channel)
        {
            case SKLegendSection::Channel::Colour:
                gl_rect_2d_in_batch(x0 + i, bot + RAMP_H, x0 + i + 1, bot, faded(rampColour(sc, t), alpha));
                break;
            case SKLegendSection::Channel::Dash:
            case SKLegendSection::Channel::Pattern:
            {
                // Dashes lengthen along the bar.
                const S32 period = 2 + (S32)(t * 10.f);
                if ((i % (period + 2)) < period)
                {
                    gl_rect_2d_in_batch(x0 + i, bot + RAMP_H / 2 + 1, x0 + i + 1, bot + RAMP_H / 2 - 1, faded(ink, alpha));
                }
                break;
            }
            default:
            {
                // A wedge: the channel grows along the bar.
                const S32 h = std::max(1, (S32)std::ceil(t * RAMP_H));
                gl_rect_2d_in_batch(x0 + i, bot + h, x0 + i + 1, bot, faded(ink, alpha));
                break;
            }
        }
    }
    auto rail = [&](F64 v, const LLColor4& c, bool caret) {
        const F64 u = rampPosition(l.section, v);
        if (std::isnan(u))
        {
            return;
        }
        const S32 x = x0 + std::min(bw - 1, (S32)(u * bw));
        gl_rect_2d_in_batch(x, bot + RAMP_H + (caret ? 0 : 2), x + 1, bot - 1, faded(c, alpha));
        if (caret)
        {
            gGL.color4fv(faded(c, alpha).mV);
            gGL.vertex2i(x - 2, bot + RAMP_H + CARET);
            gGL.vertex2i(x + 3, bot + RAMP_H + CARET);
            gGL.vertex2i(x, bot + RAMP_H);
        }
    };
    for (const SKLegendMarker& m : sec.markers)
    {
        rail(m.value, markerColour(m.kind), false);
    }
    if (mMarkScale.empty() || mMarkScale == sc.id())
    {
        rail(mMark, selected(), true);
    }
    for (const SKVizKey* k : { &mHover, &mHighlight })
    {
        if (k->scale == sc.id() && k->hasValue())
        {
            rail(k->value, ink, true);
        }
    }
    gGL.end();
}

void SKLegendView::drawRow(const Line& l, const SKLegendSection& sec, const SKLegendRow& row, F32 alpha) const
{
    const S32 mid = (l.rect.mBottom + l.rect.mTop) / 2;
    S32       x   = l.rect.mLeft;
    for (const SKLegendMark& m : row.marks)
    {
        const LLColor4 c   = faded(m.colour, alpha);
        const S32      sb  = mid - SWATCH / 2;
        const S32      st  = sb + SWATCH;
        S32            adv = SWATCH;
        gGL.getTextureSlot(0)->unbind();
        switch (m.kind)
        {
            case SKLegendMark::Kind::Swatch:
                gl_rect_2d(x, st, x + SWATCH, sb, c);
                gl_rect_2d(x, st, x + SWATCH, sb, LLColor4(0.f, 0.f, 0.f, 0.35f * alpha), false);
                break;
            case SKLegendMark::Kind::Hashed:
            {
                gl_rect_2d(x, st, x + SWATCH, sb, faded(m.colour, 0.3f * alpha));
                LLPointer<LLUIImage> hatch = LLUI::getUIImage("SkDataVizHatch");
                if (hatch.notNull())
                {
                    hatch->draw(x + 1, sb + 1, SWATCH - 2, SWATCH - 2, c);
                }
                gl_rect_2d(x, st, x + SWATCH, sb, c, false);
                break;
            }
            case SKLegendMark::Kind::Line:
            case SKLegendMark::Kind::Arrow:
            {
                adv           = LINE_MARK;
                const S32 w   = std::max(1, (S32)std::lround(m.width));
                const S32 top = mid + (w + 1) / 2;
                gl_rect_2d(x, top, x + LINE_MARK - (m.kind == SKLegendMark::Kind::Arrow ? 3 : 0), top - w, c);
                if (m.kind == SKLegendMark::Kind::Arrow)
                {
                    gl_triangle_2d(x + LINE_MARK - 4, mid + 3, x + LINE_MARK, mid, x + LINE_MARK - 4, mid - 3, c, true);
                }
                break;
            }
            case SKLegendMark::Kind::Dashed:
            {
                adv         = LINE_MARK;
                const S32 w = std::max(1, (S32)std::lround(m.width));
                for (S32 dx : { 0, 5, 10 })
                {
                    gl_rect_2d(x + dx, mid + (w + 1) / 2, x + dx + (dx == 10 ? 4 : 3), mid + (w + 1) / 2 - w, c);
                }
                break;
            }
            case SKLegendMark::Kind::Dot:
                gGL.color4fv(c.mV);
                gl_circle_2d((F32)x + SWATCH * 0.5f, (F32)mid, 2.5f, 12, true);
                break;
            case SKLegendMark::Kind::Ring:
                gGL.color4fv(c.mV);
                gl_circle_2d((F32)x + SWATCH * 0.5f, (F32)mid, 3.5f, 16, false);
                break;
            case SKLegendMark::Kind::Point:
                gl_rect_2d(x + 4, mid + 1, x + 6, mid - 1, c);
                break;
            case SKLegendMark::Kind::Cross:
                gl_line_2d(x + 1, sb + 1, x + SWATCH - 1, st - 1, c);
                gl_line_2d(x + 1, st - 1, x + SWATCH - 1, sb + 1, c);
                break;
            case SKLegendMark::Kind::BoxOutline:
                gl_rect_2d(x, st, x + SWATCH, sb, c, false);
                break;
            case SKLegendMark::Kind::Strip:
            {
                adv = std::max<S32>(1, (S32)m.strip.size()) * 4;
                for (size_t i = 0; i < m.strip.size(); ++i)
                {
                    gl_rect_2d(x + (S32)i * 4, st, x + (S32)i * 4 + 4, sb, faded(m.strip[i], alpha));
                }
                break;
            }
            case SKLegendMark::Kind::Text:
                adv = mValueFont->getWidth(m.token);
                mValueFont->renderUTF8(m.token, 0, x, l.rect.mBottom + 1, c, LLFontGL::LEFT, LLFontGL::BOTTOM);
                break;
            case SKLegendMark::Kind::Image:
            {
                LLPointer<LLUIImage> img = LLUI::getUIImage(m.token);
                if (img.notNull())
                {
                    img->draw(x, sb, SWATCH, SWATCH, faded(LLColor4::white, alpha));
                }
                break;
            }
        }
        x += adv + MARK_SPACE;
    }

    const S32 col     = mMarkColumn[l.section];
    const S32 label_x = l.rect.mLeft + (col ? col + MARK_GAP : 0);
    S32       value_w = 0;
    if (!row.value.empty())
    {
        value_w = mValueFont->getWidth(row.value);
        mValueFont->renderUTF8(row.value, 0, (F32)l.rect.mRight, (F32)(l.rect.mBottom + 1), faded(dim(), alpha), LLFontGL::RIGHT,
                               LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }
    const S32 room = l.rect.mRight - label_x - (value_w ? value_w + MARK_GAP : 0);
    if (room > 0 && !row.label.empty())
    {
        mLabelFont->renderUTF8(row.label, 0, (F32)label_x, (F32)(l.rect.mBottom + 1), faded(text(), alpha), LLFontGL::LEFT,
                               LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, room, nullptr, true);
    }
}

void SKLegendView::refreshIfScalesChanged()
{
    if (scalesRevision() != mLaidRevision)
    {
        layout();
    }
}

void SKLegendView::draw()
{
    refreshIfScalesChanged();
    const F32    alpha = getCurrentTransparency();
    const LLRect local = getLocalRect();
    gGL.getTextureSlot(0)->unbind();
    if (mFramed)
    {
        gl_rect_2d(local, faded(bg(), (mHud ? HUD_ALPHA : BG_ALPHA) * alpha));
        gl_rect_2d(local, faded(frame(), alpha), false);
    }

    const LLColor4 band = ALSurface::chosenOver(text());
    for (const Line& l : mLines)
    {
        const S32 base = l.rect.mBottom + 1;
        switch (l.kind)
        {
            case Line::Kind::Title:
                LLFontGL::getFontSansSerifSmallBold()->renderUTF8(mLegend.title, 0, (F32)l.rect.mLeft, (F32)base, faded(text(), alpha),
                    LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, l.rect.getWidth(), nullptr, true);
                break;
            case Line::Kind::Subtitle:
            case Line::Kind::Note:
            {
                const std::string& s = l.kind == Line::Kind::Note ? mLegend.note : mLegend.subtitle;
                mLabelFont->renderUTF8(s, 0, (F32)l.rect.mLeft, (F32)base, faded(dim(), alpha), LLFontGL::LEFT, LLFontGL::BOTTOM,
                                       LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, l.rect.getWidth(), nullptr, true);
                break;
            }
            case Line::Kind::Heading:
                mLabelFont->renderUTF8(headingOf(mLegend.sections[l.section]), 0, (F32)l.rect.mLeft, (F32)base, faded(dim(), alpha),
                                       LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX,
                                       l.rect.getWidth(), nullptr, true);
                break;
            case Line::Kind::Format:
                mValueFont->renderUTF8(mLegend.sections[l.section].format, 0, (F32)l.rect.mLeft, (F32)base, faded(unit(), alpha),
                                       LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX,
                                       l.rect.getWidth(), nullptr, true);
                break;
            case Line::Kind::RampBar:
                drawRamp(l, mLegend.sections[l.section], alpha);
                break;
            case Line::Kind::RampTicks:
            {
                const SKLegendSection& sec = mLegend.sections[l.section];
                const SKTicks&         t   = mTicks[l.section];
                const S32              bw  = barWidth(l.rect);
                for (size_t i = 0; i < t.values.size(); ++i)
                {
                    const bool binned = sec.scale->kind() == SKColorScale::Kind::Binned;
                    const F64  u      = binned ? (F64)i / (F64)std::max<size_t>(1, t.values.size() - 1) : sec.scale->unitOf(t.values[i]);
                    if (!(u >= -1e-9 && u <= 1.0 + 1e-9))
                    {
                        continue;
                    }
                    const S32 x = l.rect.mLeft + std::min(bw - 1, (S32)std::lround(u * (bw - 1)));
                    gGL.getTextureSlot(0)->unbind();
                    gl_rect_2d(x, l.rect.mTop, x + 1, l.rect.mTop - TICK, faded(threshold(), alpha));
                    const S32  w     = mValueFont->getWidth(t.labels[i]);
                    const S32  lx    = std::max(l.rect.mLeft, std::min(x - w / 2, l.rect.mLeft + bw - w));
                    mValueFont->renderUTF8(t.labels[i], 0, lx, base, faded(dim(), alpha));
                }
                break;
            }
            case Line::Kind::Row:
            {
                if (isRowLit(l.section, l.row))
                {
                    gGL.getTextureSlot(0)->unbind();
                    gl_rect_2d(l.rect, faded(band, alpha));
                }
                const SKLegendSection& sec = mLegend.sections[l.section];
                drawRow(l, sec, sec.rows[l.row], alpha);
                break;
            }
            case Line::Kind::More:
                mLabelFont->renderUTF8("+" + std::to_string(mHiddenRows) + " more", 0, l.rect.mLeft, base, faded(dim(), alpha));
                break;
            case Line::Kind::TooSmall:
                mLabelFont->renderUTF8("too small", 0, l.rect.mLeft, base, faded(dim(), alpha));
                break;
        }
    }
}
