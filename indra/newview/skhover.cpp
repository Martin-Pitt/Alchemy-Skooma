/**
 * @file skhover.cpp
 * @brief What the mouse is over, and what the hovered widget wants the rest of the viewer to know.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "skhover.h"

#include "llsdutil.h"
#include "lluictrl.h"
#include "llview.h"

#include <vector>

namespace
{
    struct Published
    {
        LLHandle<LLView> source;
        SKHoverContext   context;
    };

    SKHover::view_handle_set_t sHovered;
    LLHandle<LLView>           sLeaf;
    std::vector<Published>     sPublished;
    SKHoverContext             sCurrent;
    SKHover::signal_t          sChanged;
    const SKHoverContext       sNone;

    S32 depthOf(const LLView* view)
    {
        S32 depth = 0;
        for (const LLView* p = view->getParent(); p; p = p->getParent())
        {
            ++depth;
        }
        return depth;
    }
}

bool SKHoverContext::operator==(const SKHoverContext& o) const
{
    return kind == o.kind && source == o.source && llsd_equals(data, o.data);
}

void SKHover::update(const view_handle_set_t& hovered)
{
    sHovered = hovered;

    LLView* leaf = nullptr;
    S32 leaf_depth = -1;
    for (const auto& handle : sHovered)
    {
        LLView* view = handle.get();
        if (!view)
        {
            continue;
        }
        S32 depth = depthOf(view);
        if (depth > leaf_depth)
        {
            leaf = view;
            leaf_depth = depth;
        }
    }
    sLeaf = leaf ? leaf->getHandle() : LLHandle<LLView>();

    // A widget that published and then lost the cursor without a leave (hidden,
    // deleted, captured away) must not keep speaking for it.
    sPublished.erase(std::remove_if(sPublished.begin(), sPublished.end(),
                                    [](const Published& p)
                                    {
                                        return p.source.isDead() || !sHovered.count(p.source);
                                    }),
                     sPublished.end());
    refresh();
}

LLView* SKHover::leaf()
{
    return sLeaf.get();
}

bool SKHover::isHovered(const LLView* view)
{
    return view && sHovered.count(view->getHandle()) > 0;
}

bool SKHover::overUI()
{
    return !sLeaf.isDead();
}

void SKHover::publish(LLView* source, const std::string& kind, const LLSD& data)
{
    if (!source || kind.empty())
    {
        return;
    }
    LLHandle<LLView> handle = source->getHandle();
    SKHoverContext context{ kind, data, handle };
    for (Published& p : sPublished)
    {
        if (p.source == handle)
        {
            p.context = std::move(context);
            refresh();
            return;
        }
    }
    sPublished.push_back({ handle, std::move(context) });
    refresh();
}

void SKHover::retract(LLView* source)
{
    if (!source)
    {
        return;
    }
    LLHandle<LLView> handle = source->getHandle();
    sPublished.erase(std::remove_if(sPublished.begin(), sPublished.end(),
                                    [&](const Published& p) { return p.source == handle; }),
                     sPublished.end());
    refresh();
}

const SKHoverContext& SKHover::context()
{
    return sCurrent;
}

const SKHoverContext* SKHover::context(const std::string& kind)
{
    return sCurrent.kind == kind ? &sCurrent : nullptr;
}

boost::signals2::connection SKHover::listen(const signal_t::slot_type& slot)
{
    return sChanged.connect(slot);
}

void SKHover::bind(LLUICtrl* ctrl, const std::string& kind, data_fn_t data)
{
    if (!ctrl)
    {
        return;
    }
    ctrl->setMouseEnterCallback([ctrl, kind, data](LLUICtrl*, const LLSD&)
                                { SKHover::publish(ctrl, kind, data ? data() : LLSD()); });
    ctrl->setMouseLeaveCallback([ctrl](LLUICtrl*, const LLSD&) { SKHover::retract(ctrl); });
}

void SKHover::bind(LLUICtrl* ctrl, const std::string& kind, const LLSD& data)
{
    bind(ctrl, kind, [data]() { return data; });
}

// The deepest publisher wins, so a row inside a publishing panel overrides the panel.
void SKHover::refresh()
{
    const SKHoverContext* best = &sNone;
    S32 best_depth = -1;
    for (const Published& p : sPublished)
    {
        LLView* view = p.source.get();
        if (!view)
        {
            continue;
        }
        S32 depth = depthOf(view);
        if (depth > best_depth)
        {
            best = &p.context;
            best_depth = depth;
        }
    }
    if (*best != sCurrent)
    {
        sCurrent = *best;
        sChanged(sCurrent);
    }
}
