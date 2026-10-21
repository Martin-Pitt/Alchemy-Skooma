/**
 * @file skoverviewprobes.h
 * @brief The tactical cut's GPU probes: which census linksets stand round, over or under someone, by occlusion queries
 *        and a one-texel id render, and the depth under the cursor, each read back a frame or two later.
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

#ifndef SK_OVERVIEWPROBES_H
#define SK_OVERVIEWPROBES_H

#include "algpubuffer.h"
#include "alquerypool.h"
#include "llpointer.h"
#include "llrendertarget.h"
#include "lluuid.h"
#include "v3math.h"
#include "v4math.h"

#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

class LLGLSLShader;
class LLVertexBuffer;

namespace skoverview
{
    /// One person's probe of one kind: at most one in flight, answering the key it was issued for, and the key the
    /// answer standing now was for. What stands is kept until a newer answer lands, so nothing flickers between them.
    struct ProbeSlot
    {
        bool mPending = false;
        U64 mPendingKey = 0;
        U64 mSignature = 0;     ///< the census buffers' signature at issue
        U32 mIssued = 0;        ///< the probes' frame at issue
        bool mValid = false;    ///< an answer has landed
        U64 mAppliedKey = 0;    ///< the key it answers
        bool mStale = false;    ///< the probe in flight was asked before an invalidate(): applied, then asked again

        /// Whether a probe for key should be issued: none is in flight and what stands answers another key, or none.
        bool wants(U64 key) const { return !mPending && (!mValid || mAppliedKey != key); }
        void issue(U64 key, U64 signature, U32 frame);
        /// Forget what stands, and what is in flight once it lands, so the next look issues again whatever the key.
        void invalidate()
        {
            mValid = false;
            mStale = mPending;
        }

        enum class Outcome : U8
        {
            Idle,       ///< nothing in flight
            Waiting,    ///< in flight
            Apply,      ///< landed: what stands is now its answer, for its key
            Reissue,    ///< a result was lost (expired, or its readback refused): issued again by the next wants()
            Dropped,    ///< issued under buffers since replaced, whose ids it may not name: issued again likewise
        };
        /// What became of the probe in flight, from its results: all landed, any lost, and, for one that names ids, the
        /// buffers' signature now.
        Outcome settle(bool ready, bool lost, U64 signature, bool names_ids);
    };

    /// What one frame may issue: each person once against the people, each query against the queries. The first issue
    /// of a frame is always admitted, so a room of more candidates than the query budget still gets probed.
    class ProbeBudget
    {
    public:
        ProbeBudget(U32 people, U32 queries) : mPeople(people), mQueries(queries) {}
        /// Whether person may issue queries more this frame.
        bool fits(U32 person, U32 queries) const;
        /// Whether person may issue queries more this frame; counts them when it may. Called once the probe is issued,
        /// so one that could not be leaves its share to the people after it.
        bool admit(U32 person, U32 queries);
        U32 people() const { return (U32)mAdmitted.size(); }
        U32 queries() const { return mUsedQueries; }

    private:
        U32 mPeople;
        U32 mQueries;
        U32 mUsedQueries = 0;
        std::vector<U32> mAdmitted;
    };

    /// The order people are probed in: the agent first, then the rest by the frame they were last issued a probe in,
    /// longest ago first, so a budget too small for everyone serves each in turn.
    std::vector<size_t> probeOrder(const std::vector<bool>& self, const std::vector<U32>& issued);

    /// A membership probe's answer: the candidates (extent id, root) whose query let a sample through, each still the
    /// extent its root names; a root no longer present is dropped.
    std::vector<U32> passedExtents(const std::vector<std::pair<U32, LLUUID>>& candidates, const std::vector<U64>& samples,
                                   const std::map<LLUUID, U32>& root_extent);

    /// A feet texel's surface: the linkset id it shows (the texel less one) and its height between the box's near and
    /// far z, an orthographic depth being linear; false for texel 0, nothing drawn.
    bool feetSurface(U32 texel, F32 depth, F32 near_z, F32 far_z, bool reverse_z, U32& id, F32& z);
}

/// The tactical cut's questions, put to the rasteriser: membership by one exact occlusion query per candidate linkset,
/// the surface under someone's feet by a one-texel id render, and the depth under the cursor. Every answer reaches the
/// CPU a frame or two later through one pixel-pack buffer and ALGPUReadback, or as a query result, so nothing waits on
/// the GPU; GL 3.3 throughout, one path on every context. Owns one 256 x 256 GL_R32UI target with a 32-bit float depth,
/// which every probe shares through its own viewport. Main thread, render thread GL.
class SKOverviewProbes
{
public:
    static constexpr S32 SIZE = 256;

    /// A run of census vertices to draw: a page's buffer and one linkset's range in it, its region's origin from the
    /// probe's own, metres.
    struct Run
    {
        LLVertexBuffer* mBuffer = nullptr;
        U32 mFirst = 0;
        U32 mCount = 0;
        LLVector3 mOffset;
    };

    /// An orthographic look along z through a box of the probe's frame (its origin the probe's point, z up), from the
    /// plane z = mFrom to z = mTo, up when mTo is greater, onto mWidth x mHeight texels. Both planes clip exactly: what
    /// lies outside [mFrom, mTo] is never drawn.
    struct Box
    {
        F32 mMinX = 0.f;
        F32 mMinY = 0.f;
        F32 mMaxX = 0.f;
        F32 mMaxY = 0.f;
        F32 mFrom = 0.f;
        F32 mTo = 0.f;
        S32 mWidth = 1;
        S32 mHeight = 1;
        /// The projection's near and far z, a margin past each plane so the clip distances do the clipping.
        F32 nearZ() const;
        F32 farZ() const;
    };

    enum class Status : U8
    {
        Pending,
        Ready,
        Lost,   ///< its readback was refused or is too old to find
    };

    SKOverviewProbes();
    ~SKOverviewProbes();

    /// Starts a frame's probes with the id program (skoverview::PROGRAMS' "SK Overview Geometry Id"): sizes the pack
    /// buffer for samples feet texels, and takes in the readbacks that landed. False when the program or the targets
    /// are not there, and nothing may be probed this frame.
    bool beginFrame(LLGLSLShader& program, U32 samples);
    /// Requests the frame's texels back, once.
    void endFrame();
    /// Takes in the frames whose texels have landed; wait waits for every one in flight, for a teardown or a test.
    void collect(bool wait);
    U32 frame() const { return mFrame; }
    bool active() const { return mActive; }

    /// Binds the target and looks through box, its depth cleared; the outs are rects (min x, min y, max x, max y) of
    /// the box drawn into the depth first, so that nothing over them passes. Queries are begun and ended inside.
    bool beginVolume(const Box& box, const std::vector<LLVector4>& outs);
    /// Whether any sample of runs passes, as an exact occlusion query; none when the pool refused one. With edges the
    /// triangles are drawn again as lines inside the query, so a face seen edge-on (a wall from below) still counts.
    ALQueryTicket query(const std::vector<Run>& runs, bool edges);
    void endVolume();

    /// The id and depth of the nearest of runs at the box's one texel, into the frame's pack buffer: the slot to read
    /// it by, or -1 when the frame's slots are used up or the target is not there.
    S32 sample(const Box& box, const std::vector<Run>& runs);
    /// A sample's texel and window depth once it has landed.
    Status sampleResult(U32 frame, S32 slot, U32& texel, F32& depth) const;

    /// The window depth of target's texel (x, y), into the frame's pick slot.
    bool readDepth(LLRenderTarget& target, S32 x, S32 y);
    Status pickResult(U32 frame, F32& depth) const;

    /// Requests the target's last volume back whole, ids and depths, through the pack path; collectDump takes it once
    /// it has landed (or waits for it).
    bool requestDump();
    bool collectDump(std::vector<U32>& ids, std::vector<F32>& depths, S32& w, S32& h, bool wait = false);

    /// This frame's counts, for the log.
    U32 queriesIssued() const { return mQueries; }
    U32 samplesIssued() const { return mSamples; }

    void release();

    /// Which of a box's texels the rects leave out, by texel centre.
    static std::vector<bool> outTexels(const Box& box, const std::vector<LLVector4>& outs);
    /// An id render as a binary PPM, each texel scale pixels square and the top row first: each id its own colour,
    /// none near-black, the texels left out dimmed and the edge between them and the rest white.
    static bool writeImage(const std::string& path, const std::vector<U32>& ids, S32 w, S32 h, const std::vector<bool>& out,
                           S32 scale);

private:
    void bindProgram();
    /// The clip distances to the box's two planes, for what is drawn after the mask.
    void clipToBox();
    void drawRuns(const std::vector<Run>& runs);
    void beginTarget(const Box& box);
    void endTarget();

    LLRenderTarget mTarget;
    ALGPUBuffer mPack;          ///< 8 bytes a feet sample (texel, depth), then 4 for the pick
    ALGPUReadback mReadback;
    U32 mSamplesMax = 0;
    LLGLSLShader* mProgram = nullptr;
    bool mActive = false;
    bool mFailed = false;
    U32 mFrame = 0;
    U32 mSamples = 0;
    U32 mQueries = 0;
    bool mPicked = false;
    bool mInVolume = false;
    S32 mSavedViewport[4] = {};
    U32 mSavedResX = 0;
    U32 mSavedResY = 0;
    Box mVolume;                ///< the box the last volume looked through, for the dump
    LLPointer<LLVertexBuffer> mOuts; ///< the mask's rects, in the census's vertex format the id program reads
    std::deque<U32> mInFlight;  ///< the frames whose texels were requested and not yet collected, oldest first
    /// Frames whose bytes landed, newest last, a few kept.
    std::deque<std::pair<U32, std::vector<U8>>> mLanded;
    ALGPUBuffer mDumpPack;
    ALGPUReadback mDumpReadback;
    S32 mDumpW = 0;
    S32 mDumpH = 0;
};

#endif // SK_OVERVIEWPROBES_H
