/**
 * @file skworldmapsearch.cpp
 * @brief The 3D world map's region search.
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

#include "skworldmapsearch.h"

#include "lllineeditor.h"
#include "llscrolllistctrl.h"
#include "llframetimer.h"
#include "llworldmap.h"
#include "llworldmapmessage.h"
#include "skmapmode.h"
#include "skworldmapmath.h"

#include <algorithm>

namespace
{
    constexpr size_t MAX_HITS = 10;
    constexpr S32 ROW_HEIGHT = 20;
    constexpr F64 REQUEST_DELAY = 0.35;  ///< seconds after the last keystroke before the grid is asked
    constexpr size_t REQUEST_MIN_LENGTH = 3;
} // namespace

SKWorldMapSearch::SKWorldMapSearch() = default;

bool SKWorldMapSearch::postBuild()
{
    mInput = getChild<LLLineEditor>("search_input");
    mResults = getChild<LLScrollListCtrl>("search_results");
    mResultsBottom = mResults->getRect().mBottom;
    mInput->setKeystrokeCallback([](LLLineEditor*, void* data) { static_cast<SKWorldMapSearch*>(data)->onKeystroke(); }, this);
    mInput->setCommitCallback([this](LLUICtrl*, const LLSD&) { go(mSelected); });
    mResults->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        // Rows run worst at the top to best by the box.
        const S32 row = mResults->getFirstSelectedIndex();
        if (row >= 0)
        {
            go((S32)mHits.size() - 1 - row);
        }
    });
    mResults->setVisible(false);
    return true;
}

void SKWorldMapSearch::onKeystroke()
{
    std::string query = mInput->getText();
    LLStringUtil::trim(query);
    LLStringUtil::toLower(query);
    mQuery = query;
    mTypedAt = LLFrameTimer::getTotalSeconds();
    mSelected = 0;
    refilter();
}

void SKWorldMapSearch::draw()
{
    if (!mQuery.empty())
    {
        // MapNameRequest has no capability replacement; its replies land in LLWorldMap as regions.
        if (mQuery != mRequested && mQuery.size() >= REQUEST_MIN_LENGTH && LLFrameTimer::getTotalSeconds() - mTypedAt > REQUEST_DELAY)
        {
            mRequested = mQuery;
            LLWorldMapMessage::getInstance()->sendNamedRegionRequest(mQuery);
        }
        if (LLWorldMap::getInstance()->getRegionMap().size() != mKnownRegions)
        {
            refilter();
        }
    }
    LLPanel::draw();
}

void SKWorldMapSearch::refilter()
{
    const LLWorldMap::sim_info_map_t& regions = LLWorldMap::getInstance()->getRegionMap();
    mKnownRegions = regions.size();
    mHits.clear();
    if (!mQuery.empty())
    {
        for (const auto& [handle, info] : regions)
        {
            if (!info || info->getName().empty())
            {
                continue;
            }
            const F32 score = skworldmap::fuzzyScore(mQuery, info->getName());
            if (score < 0.f)
            {
                continue;
            }
            const LLVector3d centre = info->getGlobalOrigin() + LLVector3d(128.0, 128.0, 0.0);
            mHits.push_back({ info->getName(), centre, score });
        }
        std::sort(mHits.begin(), mHits.end(), [](const Hit& a, const Hit& b)
                  { return a.mScore != b.mScore ? a.mScore > b.mScore : a.mName < b.mName; });
        if (mHits.size() > MAX_HITS)
        {
            mHits.resize(MAX_HITS);
        }
    }
    mSelected = llclamp(mSelected, 0, llmax((S32)mHits.size() - 1, 0));
    showHits();
}

void SKWorldMapSearch::showHits()
{
    mResults->deleteAllItems();
    if (mHits.empty())
    {
        mResults->setVisible(false);
        return;
    }
    for (auto it = mHits.rbegin(); it != mHits.rend(); ++it)
    {
        LLSD row;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = it->mName;
        mResults->addElement(row);
    }
    LLRect rect = mResults->getRect();
    rect.setOriginAndSize(rect.mLeft, mResultsBottom, rect.getWidth(), (S32)mHits.size() * ROW_HEIGHT + 4);
    mResults->setRect(rect);
    mResults->setVisible(true);
    select(mSelected);
}

void SKWorldMapSearch::select(S32 hit)
{
    if (mHits.empty())
    {
        return;
    }
    mSelected = llclamp(hit, 0, (S32)mHits.size() - 1);
    const S32 row = (S32)mHits.size() - 1 - mSelected;
    mResults->selectNthItem(row);
    mResults->scrollToShowSelected();
}

void SKWorldMapSearch::go(S32 hit)
{
    if (hit >= 0 && hit < (S32)mHits.size())
    {
        SKMapMode::instance().flyTo(mHits[hit].mCentre);
    }
}

bool SKWorldMapSearch::handleKeyHere(KEY key, MASK mask)
{
    if (mask != MASK_NONE)
    {
        return LLPanel::handleKeyHere(key, mask);
    }
    switch (key)
    {
    case KEY_UP:     // up the list, away from the box: a worse match
        select(mSelected + 1);
        return true;
    case KEY_DOWN:
        select(mSelected - 1);
        return true;
    case KEY_ESCAPE:
        mInput->clear();
        onKeystroke();
        return true;
    default:
        return LLPanel::handleKeyHere(key, mask);
    }
}
