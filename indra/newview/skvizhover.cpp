/**
 * @file skvizhover.cpp
 * @brief Links legends to the hover context: a hovered legend row publishes its key, and a published key lights legends.
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

#include "llviewerprecompiledheaders.h"

#include "skvizhover.h"

#include "llhandle.h"
#include "lllayoutstack.h"
#include "llrootview.h"
#include "llviewerwindow.h"
#include "skhover.h"
#include "sklegendview.h"

#include <algorithm>
#include <vector>

namespace
{
    std::vector<LLHandle<LLView>>& legends()
    {
        static std::vector<LLHandle<LLView>> attached;
        return attached;
    }

    bool showsScale(const SKLegendView& v, const std::string& scale)
    {
        for (const SKLegendSection& sec : v.legend().sections)
        {
            if ((sec.scale && sec.scale->id() == scale) ||
                std::any_of(sec.rows.begin(), sec.rows.end(), [&](const SKLegendRow& r) { return r.key.scale == scale; }))
            {
                return true;
            }
        }
        return false;
    }

    void lightAll(const SKVizKey& key, const LLView* source)
    {
        auto& all = legends();
        all.erase(std::remove_if(all.begin(), all.end(), [](const LLHandle<LLView>& h) { return h.isDead(); }), all.end());
        for (const LLHandle<LLView>& h : all)
        {
            SKLegendView* v = static_cast<SKLegendView*>(h.get());
            if (v != source)
            {
                v->setHighlight(!key.empty() && showsScale(*v, key.scale) ? key : SKVizKey());
            }
        }
    }

    void listenOnce()
    {
        // Only a viz.key or the end of one touches the legends, so a world pick's highlight outlives other hovers.
        static const bool listening = (SKHover::listen([](const SKHoverContext& ctx) {
            static bool was_ours = false;
            const bool  ours     = ctx.kind == SKVizHover::KIND;
            if (ours || was_ours)
            {
                lightAll(ours ? SKVizKey::fromLLSD(ctx.data) : SKVizKey(), ours ? ctx.source.get() : nullptr);
            }
            was_ours = ours;
        }), true);
        (void)listening;
    }

    SKLegendView* hudLegend(S32 slot, LLLayoutPanel** panel)
    {
        if (!gViewerWindow || slot < 0 || slot >= SKVizHover::HUD_SLOTS)
        {
            return nullptr;
        }
        LLView* hud = gViewerWindow->getRootView()->findChildView("sk_viz_hud");
        if (!hud)
        {
            return nullptr;
        }
        *panel = hud->findChild<LLLayoutPanel>("sk_viz_hud_slot_" + std::to_string(slot));
        SKLegendView* legend = hud->findChild<SKLegendView>("sk_viz_hud_legend_" + std::to_string(slot));
        return *panel ? legend : nullptr;
    }
}

namespace SKVizHover
{
    void attach(SKLegendView* legend)
    {
        if (!legend)
        {
            return;
        }
        listenOnce();
        LLHandle<LLView> handle = legend->getHandle();
        if (std::find(legends().begin(), legends().end(), handle) != legends().end())
        {
            return;
        }
        legends().push_back(handle);
        legend->onHoverKey([handle](const SKVizKey& key) {
            if (LLView* v = handle.get())
            {
                if (key.empty())
                {
                    SKHover::retract(v);
                }
                else
                {
                    SKHover::publish(v, KIND, key.toLLSD());
                }
            }
        });
    }

    SKVizKey current()
    {
        const SKHoverContext* ctx = SKHover::context(KIND);
        return ctx ? SKVizKey::fromLLSD(ctx->data) : SKVizKey();
    }

    void highlight(const SKVizKey& key) { lightAll(key, nullptr); }

    void showInHud(S32 slot, const SKLegend& legend)
    {
        LLLayoutPanel* panel = nullptr;
        if (SKLegendView* v = hudLegend(slot, &panel))
        {
            v->setLegend(legend);
            attach(v);
            panel->setVisible(true);
        }
    }

    void hideInHud(S32 slot)
    {
        LLLayoutPanel* panel = nullptr;
        if (hudLegend(slot, &panel))
        {
            panel->setVisible(false);
        }
    }
}
