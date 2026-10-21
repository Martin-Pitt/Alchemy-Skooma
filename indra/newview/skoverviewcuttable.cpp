/**
 * @file skoverviewcuttable.cpp
 * @brief The tactical overview's per-linkset cut table.
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

#include "linden_common.h"

#include "skoverviewcuttable.h"

#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "lltimer.h"

#include <cstring>

namespace
{
    constexpr U32 CAPACITY_STEP = 1024; ///< a region's rows, rounded up to this, so a growing census rarely reallocates
}

SKOverviewCutTable::~SKOverviewCutTable()
{
    release();
}

void SKOverviewCutTable::resize(U32 rows)
{
    mRows.assign(rows, Row());
    mPending = true;
}

SKOverviewCutTable::Row& SKOverviewCutTable::row(U32 id)
{
    mEdited = true;
    return mRows[id];
}

bool SKOverviewCutTable::takePending(const Row*& rows, U32& count)
{
    if (!mPending && !mEdited)
    {
        return false;
    }
    const bool changed = mPending || mRows.size() != mTaken.size() ||
                         (!mRows.empty() && memcmp(mRows.data(), mTaken.data(), mRows.size() * sizeof(Row)) != 0);
    mPending = false;
    mEdited = false;
    if (!changed)
    {
        return false;
    }
    mTaken = mRows;
    rows = mRows.data();
    count = (U32)mRows.size();
    return true;
}

bool SKOverviewCutTable::flush()
{
    LL_PROFILE_ZONE_NAMED("sk cut table flush");
    const Row* rows = nullptr;
    U32 count = 0;
    if (!takePending(rows, count))
    {
        return false;
    }
    if (!count)
    {
        mPublishedRows = 0;
        return false;
    }
    LLTimer timer;
    if (!mStream.isAllocated() || count > mCapacity)
    {
        const U32 capacity = (count + CAPACITY_STEP - 1) / CAPACITY_STEP * CAPACITY_STEP;
        if (!mStream.allocate((size_t)capacity * sizeof(Row), 3, "SK overview cut table"))
        {
            LL_WARNS("SKOverview") << "cut table of " << capacity << " rows could not be allocated" << LL_ENDL;
            mCapacity = 0;
            mPublishedRows = 0;
            mPending = true; // tried again at the next flush
            return false;
        }
        mCapacity = capacity;
    }
    U8* dst = mStream.map();
    if (!dst)
    {
        mPending = true;
        return false;
    }
    const size_t bytes = (size_t)count * sizeof(Row);
    memcpy(dst, rows, bytes);
    mOffset = mStream.publish(bytes);
    mPublishedRows = count;
    LL_DEBUGS("SKOverview") << "cut table: " << count << " rows, " << bytes << " B published in "
                            << (S64)(timer.getElapsedTimeF64() * 1.0e6) << " us" << LL_ENDL;
    return true;
}

// static
bool SKOverviewCutTable::useStorageBlock()
{
    return LLRender::sCompute && gGLManager.mMaxVertexShaderStorageBlocks >= 1;
}

void SKOverviewCutTable::bind(LLGLSLShader& prog)
{
    if (!mPublishedRows)
    {
        return;
    }
    const S32 point = prog.getStorageBinding(LLShaderMgr::SK_OVERVIEW_CUT_BLOCK);
    if (point >= 0 && LLRender::sCompute)
    {
        // The point is the same in every cut program, so binding it again for the next is no call at all.
        gGL.bindStorageBuffer((U32)point, mStream.getBuffer(), mOffset, (size_t)mPublishedRows * sizeof(Row));
        mBoundPoint = point;
    }
}

void SKOverviewCutTable::unbind()
{
    if (mBoundPoint >= 0)
    {
        gGL.unbindStorageBuffer((U32)mBoundPoint);
        mBoundPoint = -1;
    }
}

void SKOverviewCutTable::release()
{
    unbind();
    mStream.release();
    mCapacity = 0;
    mOffset = 0;
    mPublishedRows = 0;
    mTaken.clear();
    mPending = true;
}
