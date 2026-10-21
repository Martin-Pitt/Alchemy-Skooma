/**
 * @file skoverviewprobes.cpp
 * @brief The tactical cut's GPU probes.
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

#include "skoverviewprobes.h"

#include "llgl.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llrender.h"
#include "llvertexbuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace skoverview
{
    void ProbeSlot::issue(U64 key, U64 signature, U32 frame)
    {
        mPending = true;
        mPendingKey = key;
        mSignature = signature;
        mIssued = frame;
    }

    ProbeSlot::Outcome ProbeSlot::settle(bool ready, bool lost, U64 signature, bool names_ids)
    {
        if (!mPending)
        {
            return Outcome::Idle;
        }
        if (names_ids && signature != mSignature)
        {
            mPending = false;
            mStale = false;
            return Outcome::Dropped;
        }
        if (lost)
        {
            mPending = false;
            mStale = false;
            return Outcome::Reissue;
        }
        if (!ready)
        {
            return Outcome::Waiting;
        }
        // Newer than what stands, so it is applied; asked before an invalidate(), it is asked again.
        mPending = false;
        mValid = !mStale;
        mStale = false;
        mAppliedKey = mPendingKey;
        return Outcome::Apply;
    }

    bool ProbeBudget::admit(U32 person, U32 queries)
    {
        const bool known = std::find(mAdmitted.begin(), mAdmitted.end(), person) != mAdmitted.end();
        const bool first = mAdmitted.empty() && mUsedQueries == 0;
        if (!first && ((!known && mAdmitted.size() >= mPeople) || mUsedQueries + queries > mQueries))
        {
            return false;
        }
        if (!known)
        {
            mAdmitted.push_back(person);
        }
        mUsedQueries += queries;
        return true;
    }

    std::vector<size_t> probeOrder(const std::vector<bool>& self)
    {
        std::vector<size_t> order;
        order.reserve(self.size());
        for (size_t i = 0; i < self.size(); ++i)
        {
            if (self[i])
            {
                order.push_back(i);
            }
        }
        for (size_t i = 0; i < self.size(); ++i)
        {
            if (!self[i])
            {
                order.push_back(i);
            }
        }
        return order;
    }

    std::vector<U32> passedExtents(const std::vector<std::pair<U32, LLUUID>>& candidates, const std::vector<U64>& samples,
                                   const std::map<LLUUID, U32>& root_extent)
    {
        std::vector<U32> out;
        for (size_t i = 0; i < candidates.size() && i < samples.size(); ++i)
        {
            if (!samples[i])
            {
                continue;
            }
            const auto it = root_extent.find(candidates[i].second);
            if (it != root_extent.end() && it->second == candidates[i].first)
            {
                out.push_back(candidates[i].first);
            }
        }
        return out;
    }

    bool feetSurface(U32 texel, F32 depth, F32 near_z, F32 far_z, bool reverse_z, U32& id, F32& z)
    {
        if (texel == 0)
        {
            return false;
        }
        id = texel - 1;
        const F32 d = reverse_z ? 1.f - depth : depth;
        z = near_z + d * (far_z - near_z);
        return true;
    }
}

namespace
{
    const LLStaticHashedString CLIP_PLANE("clip_plane");
    const LLStaticHashedString SK_CEILING("sk_ceiling");

    const F32 MARGIN_M = 1.f;       // how far the projection's planes lie past the box's, which clip distances keep
    const F32 OUT_DEPTH_M = 0.5f;   // where the mask's rects are drawn, on the near side of the box's first plane
    const U32 SAMPLE_BYTES = 8;     // a feet sample: its texel (U32) and its window depth (F32)
    const U32 PICK_BYTES = 4;
    const size_t LANDED_KEPT = 8;   // frames whose bytes are kept after they land

    /// The state every probe draw takes: nothing blended, culled, offset or stenciled, the four clip distances on.
    struct ProbeDrawState
    {
        LLGLDisable mBlend{ GL_BLEND };
        LLGLDisable mCull{ GL_CULL_FACE };
        LLGLDisable mOffset{ GL_POLYGON_OFFSET_FILL };
        LLGLDisable mStencil{ GL_STENCIL_TEST };
        LLGLEnable mClip0{ GL_CLIP_DISTANCE0 };
        LLGLEnable mClip1{ GL_CLIP_DISTANCE0 + 1 };
        LLGLEnable mClip2{ GL_CLIP_DISTANCE0 + 2 };
        LLGLEnable mClip3{ GL_CLIP_DISTANCE0 + 3 };
    };

    bool lookingUp(const SKOverviewProbes::Box& box)
    {
        return box.mTo > box.mFrom;
    }

    /// The eye looks along -z: up the probe's z axis, eye z is its negative; down it, eye z is it.
    LLMatrix4a viewOf(const SKOverviewProbes::Box& box)
    {
        LLMatrix4a view;
        view.setIdentity();
        if (lookingUp(box))
        {
            view.mMatrix[2].set(0.f, 0.f, -1.f, 0.f);
        }
        return view;
    }
}

F32 SKOverviewProbes::Box::nearZ() const
{
    return mFrom - (mTo > mFrom ? MARGIN_M : -MARGIN_M);
}

F32 SKOverviewProbes::Box::farZ() const
{
    return mTo + (mTo > mFrom ? MARGIN_M : -MARGIN_M);
}

SKOverviewProbes::SKOverviewProbes() = default;

SKOverviewProbes::~SKOverviewProbes() = default;

bool SKOverviewProbes::beginFrame(LLGLSLShader& program, U32 samples)
{
    ++mFrame;
    mSamples = 0;
    mQueries = 0;
    mPicked = false;
    mActive = false;
    mProgram = &program;
    if (mFailed || !program.isComplete())
    {
        return false;
    }
    if (!mTarget.isComplete() && !mTarget.allocate(SIZE, SIZE, GL_R32UI, true, false, ALTextureSlot::TT_TEXTURE,
                                                   LLRenderTarget::MIPS_NONE, LLRenderTarget::DEPTH_FMT_32F))
    {
        LL_WARNS("SKOverview") << "probe target did not allocate; the tactical cut finds no rooms, columns or roofs" << LL_ENDL;
        mFailed = true;
        return false;
    }
    samples = llmax(samples, 1u);
    if (samples != mSamplesMax || !mPack.isAllocated())
    {
        const size_t bytes = (size_t)samples * SAMPLE_BYTES + PICK_BYTES;
        mInFlight.clear();
        mLanded.clear();
        mSamplesMax = samples;
        if (!mPack.allocate(bytes, 0, nullptr, "SK overview probe texels") || !mReadback.allocate(bytes, 4, "SK overview probe readback"))
        {
            LL_WARNS("SKOverview") << "probe pack buffer did not allocate; the tactical cut finds no roofs" << LL_ENDL;
            mFailed = true;
            return false;
        }
    }
    collect(false);
    mActive = true;
    return true;
}

void SKOverviewProbes::collect(bool wait)
{
    const size_t bytes = (size_t)mSamplesMax * SAMPLE_BYTES + PICK_BYTES;
    while (!mInFlight.empty() && mReadback.getPending() > 0)
    {
        std::vector<U8> landed(bytes);
        if (!mReadback.collect(landed.data(), bytes, wait))
        {
            break;
        }
        mLanded.emplace_back(mInFlight.front(), std::move(landed));
        mInFlight.pop_front();
        while (mLanded.size() > LANDED_KEPT)
        {
            mLanded.pop_front();
        }
    }
}

void SKOverviewProbes::endFrame()
{
    if (!mActive)
    {
        return;
    }
    mActive = false;
    if (!mSamples && !mPicked)
    {
        return;
    }
    if (mReadback.request(mPack, 0, (size_t)mSamplesMax * SAMPLE_BYTES + PICK_BYTES))
    {
        mInFlight.push_back(mFrame);
    }
}

void SKOverviewProbes::bindProgram()
{
    mProgram->bind();
    const F32 none[4] = { 0.f, 0.f, 0.f, 1.f };
    glUniform4fv(mProgram->getUniformLocation(CLIP_PLANE), 1, none);
    glUniform4fv(mProgram->getUniformLocation(SK_CEILING), 1, none);
}

void SKOverviewProbes::beginTarget(const Box& box)
{
    mSavedResX = LLRenderTarget::sCurResX;
    mSavedResY = LLRenderTarget::sCurResY;
    mTarget.bindTarget();
    const S32 w = llclamp(box.mWidth, 1, SIZE);
    const S32 h = llclamp(box.mHeight, 1, SIZE);
    glViewport(0, 0, w, h);
    {
        // Only the box's texels: the rest keeps what the dump may still want.
        LLGLSScissor scissor(0, 0, w, h);
        LLGLDepthTest depth_write(GL_TRUE, GL_TRUE, GL_ALWAYS);
        gGL.setColorMask(true, true);
        const GLuint zero[4] = { 0, 0, 0, 0 };
        const GLfloat far_depth = al_window_far();
        glClearBufferuiv(GL_COLOR, 0, zero);
        glClearBufferfv(GL_DEPTH, 0, &far_depth);
    }
    const bool up = lookingUp(box);
    const F32 near_m = up ? box.nearZ() : -box.nearZ();
    const F32 far_m = up ? box.farZ() : -box.farZ();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(al_ortho(box.mMinX, box.mMaxX, box.mMinY, box.mMaxY, near_m, far_m));
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix(viewOf(box));
    // A page uploaded since the last draw may still be bound with no attributes set up for it.
    LLVertexBuffer::unbind();
    bindProgram();
    mVolume = box;
    mVolume.mWidth = w;
    mVolume.mHeight = h;
}

void SKOverviewProbes::endTarget()
{
    mProgram->unbind();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    mTarget.flush();
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LLRenderTarget::sCurResX = mSavedResX;
    LLRenderTarget::sCurResY = mSavedResY;
}

void SKOverviewProbes::drawRuns(const std::vector<Run>& runs)
{
    const LLMatrix4a view = viewOf(mVolume);
    for (const Run& run : runs)
    {
        if (!run.mBuffer || !run.mCount)
        {
            continue;
        }
        gGL.loadMatrix(view);
        gGL.translatef(run.mOffset.mV[VX], run.mOffset.mV[VY], run.mOffset.mV[VZ]);
        run.mBuffer->setBuffer();
        run.mBuffer->drawArrays(LLRender::TRIANGLES, run.mFirst, run.mCount);
    }
    gGL.loadMatrix(view);
}

bool SKOverviewProbes::beginVolume(const Box& box, const std::vector<LLVector4>& outs)
{
    if (!mActive || mInVolume)
    {
        return false;
    }
    beginTarget(box);
    mInVolume = true;
    if (outs.empty())
    {
        return true;
    }
    // The rects left out, written into the depth on the near side of the first plane: whatever lies past it there fails.
    const U32 count = (U32)outs.size() * 6;
    if (!mOuts || mOuts->getNumVerts() < count)
    {
        mOuts = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_WEIGHT | LLVertexBuffer::MAP_TEXCOORD0);
        if (!mOuts->allocateBuffer(llmax(count, 6u * 64u), 0))
        {
            mOuts = nullptr;
            return true;
        }
    }
    LLStrider<LLVector3> verts;
    LLStrider<F32> weights;
    LLStrider<LLVector2> flags;
    if (!mOuts->getVertexStrider(verts, 0, (S32)count) || !mOuts->getWeightStrider(weights, 0, (S32)count) ||
        !mOuts->getTexCoord0Strider(flags, 0, (S32)count))
    {
        mOuts->unmapBuffer();
        return true;
    }
    const F32 z = box.mFrom - (lookingUp(box) ? OUT_DEPTH_M : -OUT_DEPTH_M);
    for (const LLVector4& r : outs)
    {
        const LLVector3 a(r.mV[0], r.mV[1], z), b(r.mV[2], r.mV[1], z), c(r.mV[2], r.mV[3], z), d(r.mV[0], r.mV[3], z);
        for (const LLVector3& v : { a, b, c, a, c, d })
        {
            *verts++ = v;
            *weights++ = 0.f;
            *flags++ = LLVector2(0.f, 0.f);
        }
    }
    mOuts->unmapBuffer();
    // The upload left the buffer bound with no attributes set up for it; setBuffer sets them up only on a new binding.
    LLVertexBuffer::unbind();
    ProbeDrawState state;
    LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);
    gGL.setColorMask(false, false);
    mOuts->setBuffer();
    mOuts->drawArrays(LLRender::TRIANGLES, 0, count);
    gGL.setColorMask(true, true);
    return true;
}

void SKOverviewProbes::clipToBox()
{
    // Only what lies between the box's planes counts: eye z is minus the probe's z looking up, the probe's z looking down.
    const F32 dir = lookingUp(mVolume) ? 1.f : -1.f;
    const F32 from_plane[4] = { 0.f, 0.f, -1.f, -dir * mVolume.mFrom };
    const F32 to_plane[4] = { 0.f, 0.f, 1.f, dir * mVolume.mTo };
    glUniform4fv(mProgram->getUniformLocation(CLIP_PLANE), 1, from_plane);
    glUniform4fv(mProgram->getUniformLocation(SK_CEILING), 1, to_plane);
}

ALQueryTicket SKOverviewProbes::query(const std::vector<Run>& runs, bool edges)
{
    if (!mInVolume)
    {
        return ALQueryTicket();
    }
    clipToBox();
    ProbeDrawState state;
    LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
    const ALQueryTicket ticket = gGL.queries().begin(ALQueryKind::OcclusionExact);
    drawRuns(runs);
    if (edges)
    {
        // A face edge-on to the view covers no sample filled; its edges, as lines, cover the texels it crosses.
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        drawRuns(runs);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    gGL.queries().end(ticket);
    if (ticket.isValid())
    {
        ++mQueries;
    }
    return ticket;
}

void SKOverviewProbes::endVolume()
{
    if (!mInVolume)
    {
        return;
    }
    mInVolume = false;
    endTarget();
}

S32 SKOverviewProbes::sample(const Box& box, const std::vector<Run>& runs)
{
    if (!mActive || mInVolume || mSamples >= mSamplesMax)
    {
        return -1;
    }
    Box one = box;
    one.mWidth = 1;
    one.mHeight = 1;
    beginTarget(one);
    {
        clipToBox();
        ProbeDrawState state;
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LEQUAL);
        drawRuns(runs);
    }
    // The texel and its depth into the pack buffer, GPU-side; the binding is left empty.
    const S32 slot = (S32)mSamples++;
    const uintptr_t offset = (uintptr_t)slot * SAMPLE_BYTES;
    gGL.flush();
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, mPack.getGLName());
    glReadPixels(0, 0, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, reinterpret_cast<void*>(offset));
    glReadPixels(0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, reinterpret_cast<void*>(offset + 4));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    endTarget();
    return slot;
}

SKOverviewProbes::Status SKOverviewProbes::sampleResult(U32 frame, S32 slot, U32& texel, F32& depth) const
{
    if (slot < 0 || (U32)slot >= mSamplesMax)
    {
        return Status::Lost;
    }
    for (const auto& landed : mLanded)
    {
        if (landed.first == frame)
        {
            memcpy(&texel, landed.second.data() + (size_t)slot * SAMPLE_BYTES, 4);
            memcpy(&depth, landed.second.data() + (size_t)slot * SAMPLE_BYTES + 4, 4);
            return Status::Ready;
        }
    }
    if (frame == mFrame || std::find(mInFlight.begin(), mInFlight.end(), frame) != mInFlight.end())
    {
        return Status::Pending;
    }
    return Status::Lost;
}

bool SKOverviewProbes::readDepth(LLRenderTarget& target, S32 x, S32 y)
{
    if (!mActive || mInVolume || !target.isComplete() || !target.getDepth() || x < 0 || y < 0 || x >= (S32)target.getWidth() ||
        y >= (S32)target.getHeight())
    {
        return false;
    }
    const U32 res_x = LLRenderTarget::sCurResX;
    const U32 res_y = LLRenderTarget::sCurResY;
    target.bindTarget();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, mPack.getGLName());
    glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, reinterpret_cast<void*>((uintptr_t)mSamplesMax * SAMPLE_BYTES));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    target.flush();
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LLRenderTarget::sCurResX = res_x;
    LLRenderTarget::sCurResY = res_y;
    mPicked = true;
    return true;
}

SKOverviewProbes::Status SKOverviewProbes::pickResult(U32 frame, F32& depth) const
{
    for (const auto& landed : mLanded)
    {
        if (landed.first == frame)
        {
            memcpy(&depth, landed.second.data() + (size_t)mSamplesMax * SAMPLE_BYTES, 4);
            return Status::Ready;
        }
    }
    if (frame == mFrame || std::find(mInFlight.begin(), mInFlight.end(), frame) != mInFlight.end())
    {
        return Status::Pending;
    }
    return Status::Lost;
}

bool SKOverviewProbes::requestDump()
{
    if (!mTarget.isComplete() || mInVolume || mVolume.mWidth <= 0 || mVolume.mHeight <= 0)
    {
        return false;
    }
    const size_t bytes = (size_t)SIZE * SIZE * SAMPLE_BYTES;
    if (!mDumpPack.isAllocated() && (!mDumpPack.allocate(bytes, 0, nullptr, "SK overview probe dump") ||
                                     !mDumpReadback.allocate(bytes, 1, "SK overview probe dump readback")))
    {
        mDumpPack.release();
        return false;
    }
    if (mDumpReadback.getPending())
    {
        return false;
    }
    const S32 w = mVolume.mWidth;
    const S32 h = mVolume.mHeight;
    const U32 res_x = LLRenderTarget::sCurResX;
    const U32 res_y = LLRenderTarget::sCurResY;
    mTarget.bindTarget();
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, mDumpPack.getGLName());
    glReadPixels(0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glReadPixels(0, 0, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, reinterpret_cast<void*>((uintptr_t)w * h * 4));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    mTarget.flush();
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    LLRenderTarget::sCurResX = res_x;
    LLRenderTarget::sCurResY = res_y;
    if (!mDumpReadback.request(mDumpPack, 0, (size_t)w * h * SAMPLE_BYTES))
    {
        return false;
    }
    mDumpW = w;
    mDumpH = h;
    return true;
}

bool SKOverviewProbes::collectDump(std::vector<U32>& ids, std::vector<F32>& depths, S32& w, S32& h, bool wait)
{
    if (!mDumpReadback.isAllocated() || !mDumpReadback.getPending())
    {
        return false;
    }
    const size_t texels = (size_t)mDumpW * mDumpH;
    std::vector<U8> bytes(texels * SAMPLE_BYTES);
    if (!mDumpReadback.collect(bytes.data(), bytes.size(), wait))
    {
        return false;
    }
    ids.resize(texels);
    depths.resize(texels);
    memcpy(ids.data(), bytes.data(), texels * 4);
    memcpy(depths.data(), bytes.data() + texels * 4, texels * 4);
    w = mDumpW;
    h = mDumpH;
    return true;
}

void SKOverviewProbes::release()
{
    mTarget.release();
    mPack.release();
    mReadback.release();
    mDumpPack.release();
    mDumpReadback.release();
    mOuts = nullptr;
    mInFlight.clear();
    mLanded.clear();
    mSamplesMax = 0;
    mActive = false;
    mFailed = false;
    mInVolume = false;
    ++mFrame;
}

// static
std::vector<bool> SKOverviewProbes::outTexels(const Box& box, const std::vector<LLVector4>& outs)
{
    const S32 w = llmax(box.mWidth, 1);
    const S32 h = llmax(box.mHeight, 1);
    std::vector<bool> out((size_t)w * h, false);
    const F32 tx = (box.mMaxX - box.mMinX) / w;
    const F32 ty = (box.mMaxY - box.mMinY) / h;
    for (const LLVector4& r : outs)
    {
        const S32 x0 = llmax(0, (S32)std::ceil((r.mV[0] - box.mMinX) / tx - 0.5f));
        const S32 x1 = llmin(w - 1, (S32)std::floor((r.mV[2] - box.mMinX) / tx - 0.5f));
        const S32 y0 = llmax(0, (S32)std::ceil((r.mV[1] - box.mMinY) / ty - 0.5f));
        const S32 y1 = llmin(h - 1, (S32)std::floor((r.mV[3] - box.mMinY) / ty - 0.5f));
        for (S32 y = y0; y <= y1; ++y)
        {
            for (S32 x = x0; x <= x1; ++x)
            {
                out[(size_t)y * w + x] = true;
            }
        }
    }
    return out;
}

// static
bool SKOverviewProbes::writeImage(const std::string& path, const std::vector<U32>& ids, S32 w, S32 h, const std::vector<bool>& out,
                                  S32 scale)
{
    if (w <= 0 || h <= 0 || ids.size() < (size_t)w * h || scale <= 0)
    {
        return false;
    }
    auto outAt = [&](S32 x, S32 y) { return x >= 0 && y >= 0 && x < w && y < h && out.size() == ids.size() && out[(size_t)y * w + x]; };
    const S32 pw = w * scale;
    const S32 ph = h * scale;
    std::vector<U8> rgb((size_t)pw * ph * 3);
    for (S32 py = 0; py < ph; ++py)
    {
        const S32 y = h - 1 - py / scale;
        for (S32 px = 0; px < pw; ++px)
        {
            const S32 x = px / scale;
            const U32 id = ids[(size_t)y * w + x];
            F32 c[3] = { 16.f, 16.f, 20.f };
            if (id)
            {
                // A hue per id, far apart for neighbouring ids.
                const F32 hue = std::fmod(id * 0.618034f, 1.f) * 6.f;
                const F32 f = hue - std::floor(hue);
                const S32 i = (S32)hue % 6;
                const F32 rgb6[6][3] = { { 1, f, 0 }, { 1 - f, 1, 0 }, { 0, 1, f }, { 0, 1 - f, 1 }, { f, 0, 1 }, { 1, 0, 1 - f } };
                for (S32 k = 0; k < 3; ++k)
                {
                    c[k] = 40.f + 215.f * rgb6[i][k];
                }
            }
            const bool here = outAt(x, y);
            if (here)
            {
                for (F32& v : c)
                {
                    v = v * 0.35f + 10.f;
                }
            }
            // The mask's edge: an out texel's side that borders an in one, a pixel wide.
            const S32 sx = px % scale, sy = scale - 1 - py % scale;
            const bool edge = here && ((sx == 0 && x > 0 && !outAt(x - 1, y)) || (sx == scale - 1 && x < w - 1 && !outAt(x + 1, y)) ||
                                       (sy == 0 && y > 0 && !outAt(x, y - 1)) || (sy == scale - 1 && y < h - 1 && !outAt(x, y + 1)));
            U8* p = &rgb[((size_t)py * pw + px) * 3];
            for (S32 k = 0; k < 3; ++k)
            {
                p[k] = edge ? 255 : (U8)llclamp((S32)c[k], 0, 255);
            }
        }
    }
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }
    file << "P6\n" << pw << " " << ph << "\n255\n";
    file.write(reinterpret_cast<const char*>(rgb.data()), (std::streamsize)rgb.size());
    return (bool)file;
}
