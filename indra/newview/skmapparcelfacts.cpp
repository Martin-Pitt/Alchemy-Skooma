/**
 * @file skmapparcelfacts.cpp
 * @brief Parcel owners for the mini-map's Moderate check, from the map's own parcel requests.
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

#include "skmapparcelfacts.h"

#include "llcallbacklist.h"
#include "llparcel.h"
#include "message.h"

#include "llagent.h"
#include "llviewerregion.h"
#include "llworld.h"

namespace
{
    constexpr F64 REPLY_TIMEOUT_S = 5.0;
    constexpr size_t MAX_IN_FLIGHT = 8;
    constexpr S32 BITMAP_BYTES = skmap::PARCEL_CELLS * skmap::PARCEL_CELLS / 8;
}

SKMapParcelFacts::SKMapParcelFacts()
{
}

SKMapParcelFacts::~SKMapParcelFacts()
{
}

void SKMapParcelFacts::initSingleton()
{
    gIdleCallbacks.addFunction(&SKMapParcelFacts::onIdle, nullptr);
    if (LLWorld::instanceExists())
    {
        // Facts of a region that has gone are facts nobody can check.
        mRegionRemoved = LLWorld::getInstance()->setRegionRemovedCallback([this](LLViewerRegion* region) {
            if (region)
            {
                mCache.dropRegion(region->getRegionID());
            }
        });
    }
}

void SKMapParcelFacts::cleanupSingleton()
{
    gIdleCallbacks.deleteFunction(&SKMapParcelFacts::onIdle, nullptr);
    mRegionRemoved.disconnect();
    mPending.clear();
    mCache.clear();
}

// static
void SKMapParcelFacts::onIdle(void*)
{
    if (instanceExists())
    {
        getInstance()->expire();
    }
}

void SKMapParcelFacts::expire()
{
    const F64 now = LLFrameTimer::getUptimeSeconds().value();
    for (auto it = mPending.begin(); it != mPending.end();)
    {
        if (now - it->second.mSent >= REPLY_TIMEOUT_S)
        {
            it = mPending.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

const skmap::ParcelFacts* SKMapParcelFacts::find(const LLUUID& region, S32 x, S32 y) const
{
    return mCache.find(region, x, y, LLFrameTimer::getUptimeSeconds().value());
}

bool SKMapParcelFacts::pending(S32 seq) const
{
    const auto it = mPending.find(seq);
    return it != mPending.end() && LLFrameTimer::getUptimeSeconds().value() - it->second.mSent < REPLY_TIMEOUT_S;
}

S32 SKMapParcelFacts::request(LLViewerRegion* region, S32 x, S32 y)
{
    if (!region || !gMessageSystem)
    {
        return 0;
    }
    expire();
    for (const auto& entry : mPending)
    {
        if (entry.second.mRegion == region->getRegionID() && entry.second.mX == x && entry.second.mY == y)
        {
            return entry.first;     // already asked
        }
    }
    if (mPending.size() >= MAX_IN_FLIGHT)
    {
        return 0;
    }
    S32 seq = skmap::nextMapParcelSeq(mLastSeq);
    while (mPending.count(seq))
    {
        seq = skmap::nextMapParcelSeq(seq);
    }
    mLastSeq = seq;

    // setHoverParcel's request, for the one 4 m cell holding the target, with the map's own id.
    const F32 west = PARCEL_GRID_STEP_METERS * x;
    const F32 south = PARCEL_GRID_STEP_METERS * y;
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ParcelPropertiesRequest);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ParcelData);
    msg->addS32Fast(_PREHASH_SequenceID, seq);
    msg->addF32Fast(_PREHASH_West, west);
    msg->addF32Fast(_PREHASH_South, south);
    msg->addF32Fast(_PREHASH_East, west + PARCEL_GRID_STEP_METERS);
    msg->addF32Fast(_PREHASH_North, south + PARCEL_GRID_STEP_METERS);
    msg->addBOOLFast(_PREHASH_SnapSelection, false);
    msg->sendReliable(region->getHost());

    mPending[seq] = { region->getRegionID(), x, y, LLFrameTimer::getUptimeSeconds().value() };
    return seq;
}

// static
bool SKMapParcelFacts::handleParcelProperties(LLMessageSystem* msg, S32 seq, S32 result)
{
    if (!skmap::isMapParcelSeq(seq))
    {
        return false;
    }
    // Every id in the band is ours, even when nothing waits for it any more.
    if (instanceExists())
    {
        getInstance()->take(msg, seq, result);
    }
    return true;
}

void SKMapParcelFacts::take(LLMessageSystem* msg, S32 seq, S32 result)
{
    const auto it = mPending.find(seq);
    if (it == mPending.end())
    {
        return;     // late or a duplicate
    }
    const Pending pending = it->second;
    mPending.erase(it);
    LLViewerRegion* region = LLWorld::getInstance()->getRegion(msg->getSender());
    if (!region || region->getRegionID() != pending.mRegion)
    {
        return;
    }
    // NO_DATA is a failure, and MULTIPLE cannot describe one cell.
    if (result != PARCEL_RESULT_SUCCESS)
    {
        return;
    }
    skmap::ParcelFacts facts;
    facts.mRegion = pending.mRegion;
    msg->getS32Fast(_PREHASH_ParcelData, _PREHASH_LocalID, facts.mLocalId);
    msg->getUUIDFast(_PREHASH_ParcelData, _PREHASH_OwnerID, facts.mOwner);
    msg->getBOOLFast(_PREHASH_ParcelData, _PREHASH_IsGroupOwned, facts.mGroupOwned);
    msg->getUUIDFast(_PREHASH_ParcelData, _PREHASH_GroupID, facts.mGroup);
    facts.mTime = LLFrameTimer::getUptimeSeconds().value();
    if (msg->getSizeFast(_PREHASH_ParcelData, _PREHASH_Bitmap) == BITMAP_BYTES)
    {
        // The parcel's own cells: the entry answers for every one of them.
        U8 bitmap[BITMAP_BYTES];
        msg->getBinaryDataFast(_PREHASH_ParcelData, _PREHASH_Bitmap, bitmap, BITMAP_BYTES);
        if (!skmap::parcelBitmapHas(bitmap, pending.mX, pending.mY))
        {
            return;
        }
        for (S32 y = 0; y < skmap::PARCEL_CELLS; ++y)
        {
            for (S32 x = 0; x < skmap::PARCEL_CELLS; ++x)
            {
                if (skmap::parcelBitmapHas(bitmap, x, y))
                {
                    facts.mCells.set(static_cast<size_t>(x + skmap::PARCEL_CELLS * y));
                }
            }
        }
    }
    else
    {
        facts.mCells.set(static_cast<size_t>(pending.mX + skmap::PARCEL_CELLS * pending.mY));
    }
    mCache.insert(facts);
}
