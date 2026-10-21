/**
 * @file skoverviewcuttable.h
 * @brief The tactical overview's per-linkset cut table: one row per census linkset, published to the GPU whole.
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

#ifndef SK_OVERVIEWCUTTABLE_H
#define SK_OVERVIEWCUTTABLE_H

#include "algpubuffer.h"
#include "stdtypes.h"

#include <vector>

/// The cut of every census linkset, by its id: SKOverviewCensus::Group::mId, dense in [0, rows()) over one build of
/// every region, stored per vertex as a float in the census buffer's weight channel and read in overviewGeomV.glsl as
/// int(weight + 0.5). A row is 16 bytes, a vec4 to the shader: x the cut (agent z, WHOLE or HIDDEN), y flags (none
/// assigned yet), z and w the linkset's lowest and highest triangle, global z.
///
/// The rows are published whole through one ALGPUStream, three regions of them, so a rewrite never touches what a draw
/// still reads. The vertex stage reads them from one of two stores, chosen per context:
/// - SK_CUT_TABLE_SSBO, under LLRender::sCompute: the storage block SKOverviewCutBlock { vec4 rows[]; }, the reserved
///   block LLShaderMgr::SK_OVERVIEW_CUT_BLOCK at binding point 0;
/// - otherwise (GL 4.1): samplerBuffer sk_cut_table (the reserved uniform LLShaderMgr::SK_OVERVIEW_CUT_TABLE, a
///   GL_RGBA32F buffer texture over the stream's whole buffer) at texel sk_cut_base + id.
/// Main thread; flush, release and the binds need a current GL context, the rest none.
class SKOverviewCutTable
{
public:
    static constexpr F32 WHOLE = 1.0e30f;   ///< a linkset drawn whole
    static constexpr F32 HIDDEN = -1.0e30f; ///< a linkset not drawn at all

    struct Row
    {
        F32 mCut = WHOLE;
        F32 mFlags = 0.f;
        F32 mMinZ = 0.f;
        F32 mMaxZ = 0.f;
    };
    static_assert(sizeof(Row) == 16, "a row is one vec4");

    SKOverviewCutTable() = default;
    ~SKOverviewCutTable();

    SKOverviewCutTable(const SKOverviewCutTable&) = delete;
    SKOverviewCutTable& operator=(const SKOverviewCutTable&) = delete;

    /// Every row WHOLE, and pending until its first publish.
    void resize(U32 rows);
    /// A row to edit; an edit is published by the next flush that finds the rows changed.
    Row& row(U32 id);
    const Row& row(U32 id) const { return mRows[id]; }
    U32 rows() const { return (U32)mRows.size(); }

    /// The rows to publish, once per change: true when they differ from those taken last, or were resized since.
    bool takePending(const Row*& rows, U32& count);
    /// takePending, then the rows into the stream's next region; true when it published.
    bool flush();

    /// The buffer and offset of the rows published last, rows() of them; for tests and readers of the store.
    const ALGPUBuffer& buffer() const { return mStream.getBuffer(); }
    size_t offset() const { return mOffset; }
    U32 publishedRows() const { return mPublishedRows; }

    /// Releases the GL objects and forgets what was published.
    void release();

private:
    std::vector<Row> mRows;
    std::vector<Row> mTaken;    ///< the rows takePending handed out last
    bool mPending = true;       ///< resized since the last takePending
    bool mEdited = false;       ///< row() was asked for since the last takePending
    ALGPUStream mStream;
    U32 mCapacity = 0;          ///< rows a region holds
    size_t mOffset = 0;         ///< where the rows published last start in the stream's buffer
    U32 mPublishedRows = 0;
};

#endif
