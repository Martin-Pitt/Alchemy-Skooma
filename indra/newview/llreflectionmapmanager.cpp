/**
 * @file llreflectionmapmanager.cpp
 * @brief LLReflectionMapManager class implementation
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
 *
 * Alchemy Viewer Source Code
 * Copyright © 2026, Rye <rye@alchemyviewer.org>
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

#include "llreflectionmapmanager.h"

#include <chrono>
#include <vector>

#include "llviewercamera.h"
#include "llspatialpartition.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "llviewershadermgr.h"
#include "llviewercontrol.h"
#include "llenvironment.h"
#include "llstartup.h"
#include "llviewermenufile.h"
#include "llnotificationsutil.h"
#include "llagent.h"
#include "sknavmesh.h"
#include "sknavrooms.h"
#include "sknavprobes.h"
#include "skclusterprobes.h" // <SK:Nexii> Cluster probes

#if LL_WINDOWS
#pragma warning (push)
#pragma warning (disable : 4702) // compiler complains unreachable code
#endif
#define TINYEXR_USE_MINIZ 0
#include <zlib.h>
#include <tinyexr.h>
#if LL_WINDOWS
#pragma warning (pop)
#endif

LLPointer<LLImageGL> gEXRImage;

void load_exr(const std::string& filename)
{
    // reset reflection maps when previewing a new HDRI
    gPipeline.mReflectionMapManager.reset();
    gPipeline.mReflectionMapManager.initReflectionMaps();

    float* out; // width * height * RGBA
    int width;
    int height;
    const char* err = NULL; // or nullptr in C++11

    int ret =  LoadEXRWithLayer(&out, &width, &height, filename.c_str(), /* layername */ nullptr, &err);
    if (ret == TINYEXR_SUCCESS)
    {
        U32 texName = 0;
        LLImageGL::generateTextures(1, &texName);

        gEXRImage = new LLImageGL(texName, 4, GL_TEXTURE_2D, GL_RGB16F, GL_RGBA, GL_FLOAT);
        gEXRImage->setHasMipMaps(true);
        gEXRImage->setUseMipMaps(true);
        gEXRImage->markStorageAllocated();

        gGL.getTextureSlot(0)->bind(gEXRImage);

        // Immutable storage with the full mip chain, upload and VRAM accounting in one
        // call -- this used to be a raw mutable glTexImage2D bypassing the allocator.
        LLImageGL::allocateTexture2D(GL_TEXTURE_2D, GL_RGB16F, width, height, GL_RGBA, GL_FLOAT, out,
                                     LLImageGL::calcMipLevelCount(width, height));

        free(out); // release memory of image data

        LLImageGL::generateMipmaps(GL_TEXTURE_2D);

        gGL.getTextureSlot(0)->unbind();

    }
    else
    {
        LLSD notif_args;
        notif_args["WHAT"] = filename;
        notif_args["REASON"] = "Unknown";
        if (err)
        {
            notif_args["REASON"] = std::string(err);
            FreeEXRErrorMessage(err); // release memory of error message.
        }
        LLNotificationsUtil::add("CannotLoad", notif_args);
    }
}

void hdri_preview()
{
    LLFilePickerReplyThread::startPicker(
        [](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter load_filter, LLFilePicker::ESaveFilter save_filter)
        {
            if (LLAppViewer::instance()->quitRequested())
            {
                return;
            }
            if (filenames.size() > 0)
            {
                load_exr(filenames[0]);
            }
        },
        LLFilePicker::FFLOAD_HDRI,
        true);
}

extern bool gCubeSnapshot;
extern bool gTeleportDisplay;

static U32 sUpdateCount = 0;

#if !LL_RELEASE_FOR_DOWNLOAD
namespace
{
    // Register the tail of the ReflectionProbes block for debug validation at shader load.
    // Only the scalars are listed: the leading arrays are what fixes their offsets, so a
    // packing surprise anywhere ahead of them shows up here, and these are the members with no
    // other check -- the C++ struct's own size is the only thing pinning _ssrTailPad.
    const bool s_probe_layout_registered = []
    {
#define LL_PROBE_LAYOUT(m) \
        { -1, #m, (U32)offsetof(LLReflectionMapManager::ReflectionProbeData, m), false }
        LLGLSLShader::registerEngineBlockLayout("ReflectionProbes",
        {
            LL_PROBE_LAYOUT(refmapCount),
            LL_PROBE_LAYOUT(heroShape),
            LL_PROBE_LAYOUT(heroMipCount),
            LL_PROBE_LAYOUT(heroProbeCount),
            LL_PROBE_LAYOUT(iterationCount),
            LL_PROBE_LAYOUT(rayStep),
            LL_PROBE_LAYOUT(distanceBias),
            LL_PROBE_LAYOUT(depthRejectBias),
            LL_PROBE_LAYOUT(glossySampleCount),
            LL_PROBE_LAYOUT(adaptiveStepMultiplier),
        });
#undef LL_PROBE_LAYOUT
        return true;
    }();
}
#endif // !LL_RELEASE_FOR_DOWNLOAD

// get the next highest power of two of v (or v if v is already a power of two)
//defined in llvertexbuffer.cpp
extern U32 nhpo2(U32 v);

static void touch_default_probe(LLReflectionMap* probe)
{
    if (LLViewerCamera::getInstance())
    {
        LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
        origin.mV[2] += 64.f;

        probe->mOrigin.load3(origin.mV);
    }
}

LLReflectionMapManager::LLReflectionMapManager()
: mSolids(new sknav::SolidLattice),
  mClusterProbes(std::make_unique<SKClusterProbes>()) // <SK:Nexii> Cluster probes
{
    mDynamicProbeCount = LL_MAX_REFLECTION_PROBE_COUNT;
    initCubeFree();
}

// Defined here, where the forward-declared SolidLattice and census snapshot
// the members hold are complete types.
LLReflectionMapManager::~LLReflectionMapManager() = default;

void LLReflectionMapManager::initCubeFree()
{
    // start at 1 because index 0 is reserved for mDefaultProbe
    for (U32 i = 1; i < mDynamicProbeCount; ++i)
    {
        mCubeFree.push_back(i);
    }
}

// Order probes for the cube-slot handout below: placed probes first, then by distance.
//
// This order decides which probes exist in VRAM at all, so it is where artist intent has to be
// honoured. Distance alone did the opposite: mDistance is (distance to origin - radius), so a
// large influence volume reads as nearer than a small one the camera is equally far inside --
// an automatic probe covering a 16m octree node has a radius of about 13.9, and a manual probe
// on a 2m prim standing in the same spot has 1. The placed probe lost the slot every time.
//
// A probe's radius is not a statement about its importance when the octree chose it and is
// exactly that when a person did, so priority leads and distance orders within each group.
// Automatic probes are a fallback for space nobody placed a probe in; they should be what gives
// when there is not enough room for everything.
struct CompareProbeDistance
{
    bool operator()(const LLPointer<LLReflectionMap>& lhs, const LLPointer<LLReflectionMap>& rhs) const
    {
        if ((lhs->mDistance == FLT_MAX) != (rhs->mDistance == FLT_MAX))
        {
            return rhs->mDistance == FLT_MAX;
        }
        if (lhs->mPriority != rhs->mPriority)
        {
            return lhs->mPriority > rhs->mPriority;
        }

        if (lhs->mClusterFill != rhs->mClusterFill) // <SK:Nexii> Cluster probes: few and broad, so they keep their slots first
        {
            return lhs->mClusterFill;
        }
        if (lhs->mNavRooms != rhs->mNavRooms)
        {
            return lhs->mNavRooms;
        }
        return lhs->mDistance < rhs->mDistance;
    }
};

static F32 update_score(LLReflectionMap* p)
{
    return (F32)(gFrameTimeSeconds - p->mLastUpdateTime) - p->mDistance*0.1f;
}

// return true if a is higher priority for an update than b
static bool check_priority(LLReflectionMap* a, LLReflectionMap* b)
{
    if (a->mCubeIndex == -1)
    { // not a candidate for updating
        return false;
    }
    else if (b->mCubeIndex == -1)
    { // b is not a candidate for updating, a is higher priority by default
        return true;
    }
    else if (!a->mComplete && !b->mComplete)
    { //neither probe is complete, generate the placed one first, then use distance
        if (a->mPriority != b->mPriority)
        {
            return a->mPriority > b->mPriority;
        }
        if (a->mClusterFill != b->mClusterFill) // <SK:Nexii> Cluster probes: one capture covers a whole structure
        {
            return a->mClusterFill;
        }
        if (a->mNavRooms != b->mNavRooms)
        {
            return a->mNavRooms;
        }
        return a->mDistance < b->mDistance;
    }
    else if (a->mComplete && b->mComplete)
    { //both probes are complete, use update_score metric
        return update_score(a) > update_score(b);
    }

    // a or b is not complete,
    if (sUpdateCount % 3 == 0)
    { // every third update, allow complete probes to cut in line in front of non-complete probes to avoid spammy probe generators from deadlocking scheduler (SL-20258))
        return !b->mComplete;
    }

    // prioritize incomplete probe
    return b->mComplete;
}

// helper class to seed octree with probes
void LLReflectionMapManager::update()
{
    if (!LLPipeline::sReflectionProbesEnabled || gTeleportDisplay || LLStartUp::getStartupState() < STATE_PRECACHE)
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("reflection manager update");
    llassert(!gCubeSnapshot); // assert a snapshot is not in progress
    if (LLAppViewer::instance()->logoutRequestSent())
    {
        return;
    }

    if (mPaused && gFrameTimeSeconds > mResumeTime)
    {
        resume();
    }

    mResetFade = llmin((F32)(mResetFade + gFrameIntervalSeconds * 2.f), 1.f);

    {
        U32 probe_count_temp = mDynamicProbeCount;
        if (mRenderReflectionProbeDynamicAllocation > -1)
        {
            if (mRenderReflectionProbeLevel == 0)
            {
                mDynamicProbeCount = 1;
            }
            else if (mRenderReflectionProbeLevel == 1)
            {
                mDynamicProbeCount = (U32)mProbes.size();
            }
            else if (mRenderReflectionProbeLevel == 2)
            {
                mDynamicProbeCount = llmax((U32)mProbes.size(), 128);
            }
            else
            {
                mDynamicProbeCount = 256;
            }

            if (mRenderReflectionProbeDynamicAllocation > 1)
            {
                // Round mDynamicProbeCount to the nearest increment of 16
                mDynamicProbeCount = ((mDynamicProbeCount + mRenderReflectionProbeDynamicAllocation / 2) / mRenderReflectionProbeDynamicAllocation) * 16;
                mDynamicProbeCount = llclamp(mDynamicProbeCount, 1, mRenderReflectionProbeCount);
            }
            else
            {
                mDynamicProbeCount = llclamp(mDynamicProbeCount + mRenderReflectionProbeDynamicAllocation, 1, mRenderReflectionProbeCount);
            }
        }
        else
        {
            mDynamicProbeCount = mRenderReflectionProbeCount;
        }

        mDynamicProbeCount = llmin(mDynamicProbeCount, LL_MAX_REFLECTION_PROBE_COUNT);

        if (mDynamicProbeCount != probe_count_temp)
            mResetFade = 1.f;
    }

    initReflectionMaps();

    static LLCachedControl<bool> render_hdr(gSavedSettings, "RenderHDREnabled", true);

    if (!mRenderTarget.isComplete())
    {
        U32 color_fmt = render_hdr ? GL_R11F_G11F_B10F : GL_RGB8;
        U32 targetRes = mProbeResolution * mSuperSample; // super sample
        mRenderTarget.allocate(targetRes, targetRes, color_fmt, true);
    }

    if (mMipChain.empty())
    {
        U32 res = mProbeResolution;
        U32 count = (U32)(log2((F32)res) + 0.5f);

        mMipChain.resize(count);
        for (U32 i = 0; i < count; ++i)
        {
            mMipChain[i].allocate(res, res, render_hdr ? GL_R11F_G11F_B10F : GL_RGB8);
            res /= 2;
        }
    }

    {
        // Scratch for the row-parallel SH projection in updateProbeFace: nine coefficients wide,
        // one row per face row of the mip it integrates. Sized here, outside the reset guard in
        // initReflectionMaps, so a change of ALProbeSHProjectionRes resizes this and nothing else.
        static LLCachedControl<bool> two_pass(gSavedSettings, "ALProbeSHProjectionTwoPass", true);
        S32 sh_mip = 0;
        const U32 sh_rows = two_pass ? 6 * shProjectionRes(sh_mip) : 0;
        if (mSHPartial.isComplete() && mSHPartial.getHeight() != sh_rows)
        {
            mSHPartial.release();
        }
        if (sh_rows > 0 && !mSHPartial.isComplete())
        {
            mSHPartial.allocate(LL_SH_COEFF_COUNT, sh_rows, GL_RGBA16F);
        }
    }

    llassert(mProbes[0] == mDefaultProbe);

    LLVector4a camera_pos;
    camera_pos.load3(LLViewerCamera::instance().getOrigin().mV);

    // process kill list
    for (auto& probe : mKillList)
    {
        auto const & iter = std::find(mProbes.begin(), mProbes.end(), probe);
        if (iter != mProbes.end())
        {
            deleteProbe((U32)(iter - mProbes.begin()));
        }
    }

    mKillList.clear();

    // process create list
    for (auto& probe : mCreateList)
    {
        mProbes.push_back(probe);
    }

    mCreateList.clear();

    if (mProbes.empty())
    {
        return;
    }


    bool did_update = false;

    bool realtime = mRenderReflectionProbeDetail >= (S32)LLReflectionMapManager::DetailLevel::REALTIME;

    LLReflectionMap* closestDynamic = nullptr;

    LLReflectionMap* oldestProbe = nullptr;
    LLReflectionMap* oldestOccluded = nullptr;

    SKNavMesh::instance().placeReflectionProbes();
    updateCensusPlacement();
    updateClusterProbes(); // <SK:Nexii> Cluster probes

    // Occlusion state only means anything for as long as the queries that produce it keep
    // running, and they stop whenever LLPipeline::sUseOcclusion drops out of its "query"
    // level -- the UseOcclusion setting, a feature-table veto, or simply turning on wireframe.
    // Nothing used to clear mOccluded when that happened, so every probe that was occluded at
    // that instant stayed occluded for the rest of the session: dropped by getReflectionMaps,
    // skipped by the neighbour packer, and unable to recover, because recovering needs a query
    // result and no query would ever run again. The one probe that escaped was whichever one
    // the camera stood inside, since that path answers false before issuing anything.
    if (LLPipeline::sUseOcclusion <= 1)
    {
        for (auto& probe : mProbes)
        {
            probe->mOccluded = false;
        }
    }

    // Make probes track the viewer objects they are attached to, then work out which automatic
    // probes a manual probe has made redundant.
    //
    // An automatic probe is a fallback for space nobody placed a probe in. Where a placed probe
    // has swallowed its whole influence volume it can no longer change a single pixel -- see
    // eclipses() for what "swallowed" has to mean for that to be true of each volume shape --
    // so the cube slot and the place in the update queue it holds are pure waste. Only probes
    // that are actually rendering get to evict anything, or a probe still generating would
    // leave a hole where the one it displaced used to be.
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmu - manual eclipse");

        // Slack, applied in whichever direction preserves the current state: a probe sitting on
        // the boundary of a manual probe that is moving would otherwise flip every frame, and
        // each flip costs it a full twelve-pass regeneration.
        const F32 ECLIPSE_HYSTERESIS = 0.5f;

        std::vector<LLReflectionMap*> manual_probes;

        for (auto& probe : mProbes)
        {
            probe->syncToViewerObject();

            if (probe != mDefaultProbe && probe->mViewerObject && probe->mComplete &&
                probe->mCubeIndex != -1 && probe->mFadeIn >= 1.f && probe->isRelevant())
            {
                manual_probes.push_back(probe);
            }
        }

        for (auto& probe : mProbes)
        {
            if (probe == mDefaultProbe || probe->mViewerObject)
            { // only automatic probes are ever displaced
                continue;
            }

            F32 margin = probe->mInsideManualProbe ? ECLIPSE_HYSTERESIS : -ECLIPSE_HYSTERESIS;

            bool eclipsed = false;
            for (auto* manual : manual_probes)
            {
                if (manual->eclipses(probe, margin))
                {
                    eclipsed = true;
                    break;
                }
            }

            probe->mInsideManualProbe = eclipsed;

        }
    }

    updateRelevanceLinks(); // <SK:Nexii> Probe links: relevance flips (eclipse, census, nav rooms) rebuild the graph

    // A probe that stopped being relevant partway through its twelve passes is dropped rather than
    // finished, by the same rule the scheduling loop below uses to skip one. Every remaining pass
    // renders the whole scene for a probe that is about to lose its cube slot in the release pass,
    // or, when whatever registered it is gone, to be deleted outright. It comes after the eclipse
    // pass above so that it judges on this frame's mInsideManualProbe, as the scheduling loop does:
    // judged on last frame's, a probe whose eclipser had just gone would be abandoned here, found
    // relevant again a few lines later, and start its twelve passes over.
    if (mUpdatingProbe != nullptr && mUpdatingProbe != mDefaultProbe && !mUpdatingProbe->isRelevant())
    {
        abandonProbeUpdate();
    }

    if (mUpdatingProbe != nullptr)
    {
        did_update = true;
        doProbeUpdate();
    }

    // Update distance to camera for all probes.
    //
    // This has to happen before the sort below, before the cube slot handout that follows it,
    // and before check_priority's tie-break -- all three consume mDistance. It used to be done
    // inside the scheduling loop further down, one frame after the sort that reads it, and only
    // for probes that loop did not skip: an irrelevant probe, or any probe at all while paused,
    // kept whatever value it last held and sorted on that. A newly created probe holds -1, which
    // is nearer than anything real.
    for (auto& probe : mProbes)
    {
        if (probe == mDefaultProbe)
        {
            // 64m for the purposes of prioritization once it has been generated once, and a
            // hard boost while it has not
            probe->mDistance = probe->mComplete ? 64.f : -4096.f;
            continue;
        }

        if (!probe->isRelevant())
        { // sorts to the very back, so the release pass below reclaims its cube slot -- that
          // pass only ever looks past mReflectionProbeCount, so a probe that stopped mattering
          // while close to the camera used to hold its slot indefinitely
            probe->mDistance = FLT_MAX;
            continue;
        }

        LLVector4a d;
        d.setSub(camera_pos, probe->mOrigin);
        probe->mDistance = d.getLength3().getF32() - probe->mRadius;
    }

    std::sort(mProbes.begin()+1, mProbes.end(), CompareProbeDistance());
    llassert(mProbes[0] == mDefaultProbe);
    llassert(mProbes[0]->mCubeArray == mTexture);
    llassert(mProbes[0]->mCubeIndex == 0);

    // make sure we're assigning cube slots to the closest probes

    // first free any cube indices for distant probes, and for probes that can no longer affect
    // anything. The second case was unreachable before: relevance had no bearing on the sort, so
    // an irrelevant probe near the camera sat inside the budget and held its slot indefinitely,
    // while still being complete enough for getReflectionMaps to keep handing it to the shader.
    // Index 0 is the default probe and is never released.
    for (U32 i = 1; i < mProbes.size(); ++i)
    {
        LLReflectionMap* probe = mProbes[i];
        llassert(probe != nullptr);

        if (probe && probe->mCubeIndex != -1 && mUpdatingProbe != probe &&
            (i >= mReflectionProbeCount || !probe->isRelevant()))
        { // free this index
            mCubeFree.push_back(probe->mCubeIndex);

            probe->mCubeArray = nullptr;
            probe->mCubeIndex = -1;
            probe->mComplete = false;
            probe->mFadeIn = 0;
            // Cleared with the rest of the render state. A probe that gets a slot back later
            // would otherwise start out culled by a query taken back when it still had
            // something to show, and need two more occlusion passes to say so -- on top of the
            // twelve it needs to regenerate.
            probe->mOccluded = false;
        }
    }

    // next distribute the free indices
    U32 count = llmin(mReflectionProbeCount, (U32)mProbes.size());

    for (U32 i = 1; i < count && !mCubeFree.empty(); ++i)
    {
        // find the closest probe that needs a cube index
        LLReflectionMap* probe = mProbes[i];

        // Relevance checked here as well as in the release pass above. When there are fewer
        // probes than slots every probe is inside the budget, so without this an irrelevant one
        // would be handed a slot and have it taken back on the next frame, forever.
        if (probe->mCubeIndex == -1 && probe->isRelevant())
        {
            S32 idx = allocateCubeIndex();
            llassert(idx > 0); //if we're still in this loop, mCubeFree should not be empty and allocateCubeIndex should be returning good indices
            probe->mCubeArray = mTexture;
            probe->mCubeIndex = idx;
        }
    }

    for (unsigned int i = 0; i < mProbes.size(); ++i)
    {
        LLReflectionMap* probe = mProbes[i];
        if (probe->getNumRefs() == 1)
        { // no references held outside manager, delete this probe
            deleteProbe(i);
            --i;
            continue;
        }

        if (probe != mDefaultProbe &&
            (!probe->isRelevant() || mPaused))
        { // skip irrelevant probes (or all non-default probes if paused)
            continue;
        }

        if (probe->mComplete)
        {
            probe->autoAdjustOrigin();
            probe->mFadeIn = llmin((F32) (probe->mFadeIn + gFrameIntervalSeconds), 1.f);

            // Rebuild the neighbour graph once the influence volume has drifted away from the
            // one it was built against.
            //
            // The shader stops its scan at the first probe covering a pixel and then considers
            // only that probe's neighbours, which is sound only while the graph is current: two
            // probes that both contain a point necessarily intersect, so the second is reachable
            // from the first. The graph was only ever rebuilt when a probe finished a full
            // twelve-pass update, so a probe riding a moving object described where it used to
            // be, and a pixel it covered could be skipped outright because whichever probe the
            // scan started from had never heard of it. Rebuilding here also fixes the other
            // side, since the search phase writes into both lists.
            if (probe->neighborsAreStale())
            {
                updateNeighbors(probe);
            }
        }
        if (probe->mOccluded && probe->mComplete)
        {
            if (oldestOccluded == nullptr)
            {
                oldestOccluded = probe;
            }
            else if (probe->mLastUpdateTime < oldestOccluded->mLastUpdateTime)
            {
                oldestOccluded = probe;
            }
        }
        else
        {
            if (!did_update &&
                i < mReflectionProbeCount &&
                (oldestProbe == nullptr ||
                    check_priority(probe, oldestProbe)))
            {
               oldestProbe = probe;
            }
        }

        if (realtime &&
            closestDynamic == nullptr &&
            probe->mCubeIndex != -1 &&
            probe->getIsDynamic())
        {
            closestDynamic = probe;
        }

        if (mRenderReflectionProbeLevel == 0)
        {
            // only update default probe when coverage is set to none
            llassert(probe == mDefaultProbe);
            break;
        }
    }

    if (realtime && closestDynamic != nullptr)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmu - realtime");
        // update the closest dynamic probe realtime
        // should do a full irradiance pass on "odd" frames and a radiance pass on "even" frames
        closestDynamic->autoAdjustOrigin();

        // store and override the value of "isRadiancePass" -- parts of the render pipe rely on "isRadiancePass" to set
        // lighting values etc
        bool radiance_pass = isRadiancePass();
        mRadiancePass = mRealtimeRadiancePass;
        for (U32 i = 0; i < 6; ++i)
        {
            updateProbeFace(closestDynamic, i);
        }
        mRealtimeRadiancePass = !mRealtimeRadiancePass;

        // restore "isRadiancePass"
        mRadiancePass = radiance_pass;
    }

    static LLCachedControl<F32> sUpdatePeriod(gSavedSettings, "RenderDefaultProbeUpdatePeriod", 2.f);
    if ((gFrameTimeSeconds - mDefaultProbe->mLastUpdateTime) < sUpdatePeriod)
    {
        if (mRenderReflectionProbeLevel == 0)
        { // when probes are disabled don't update the default probe more often than the prescribed update period
            oldestProbe = nullptr;
        }
    }
    else if (mRenderReflectionProbeLevel > 0)
    { // when probes are enabled don't update the default probe less often than the prescribed update period
      oldestProbe = mDefaultProbe;
    }

    // switch to updating the next oldest probe
    if (!did_update && oldestProbe != nullptr)
    {
        LLReflectionMap* probe = oldestProbe;
        llassert(probe->mCubeIndex != -1);

        // <SK:Nexii> Navmesh rooms probes: a probe the census pass has anchored gets its
        // placement from that pass -- re-deriving it here would reset the radius the pass
        // cleared and restart the capture on every refresh.
        if (!probe->mGridPlacementChecked)
        {
            probe->autoAdjustOrigin();
        }
        // </SK:Nexii>

        sUpdateCount++;
        mUpdatingProbe = probe;
        doProbeUpdate();
    }

    if (oldestOccluded)
    {
        // as far as this occluded probe is concerned, an origin/radius update is as good as a full update
        oldestOccluded->autoAdjustOrigin();
        oldestOccluded->mLastUpdateTime = gFrameTimeSeconds;
    }
}

void LLReflectionMapManager::refreshSettings()
{
    mRenderReflectionProbeDetail = gSavedSettings.getS32("RenderReflectionProbeDetail");
    mRenderReflectionProbeLevel = gSavedSettings.getS32("RenderReflectionProbeLevel");
    mRenderReflectionProbeCount = gSavedSettings.getU32("RenderReflectionProbeCount");
    mRenderReflectionProbeDynamicAllocation = gSavedSettings.getS32("RenderReflectionProbeDynamicAllocation");
    mNavRoomsDirty = true; // <SK:Nexii> Navmesh rooms probes
    cleanupQueryPool();
}

LLReflectionMap* LLReflectionMapManager::addProbe(LLSpatialGroup* group)
{
    if (gGLManager.mGLVersion < 4.05f || !LLPipeline::sReflectionProbesEnabled)
    {
        return nullptr;
    }

    LLReflectionMap* probe = new LLReflectionMap();
    probe->mGroup = group;

    if (mDefaultProbe.isNull())
    {  //safety check to make sure default probe is always first probe added
        mDefaultProbe = new LLReflectionMap();
        mProbes.push_back(mDefaultProbe);
    }

    llassert(mProbes[0] == mDefaultProbe);

    if (group)
    {
        probe->mOrigin = group->getOctreeNode()->getCenter();
        mNavRoomsDirty = true; // <SK:Nexii> Navmesh rooms probes
    }

    if (gCubeSnapshot)
    { //snapshot is in progress, mProbes is being iterated over, defer insertion until next update
        mCreateList.push_back(probe);
    }
    else
    {
        mProbes.push_back(probe);
    }

    return probe;
}

U32 LLReflectionMapManager::probeCount()
{
    return mDynamicProbeCount;
}

U32 LLReflectionMapManager::probeMemory()
{
    // The SH coefficient strip is 9 x probes x RGBA16F -- tens of kilobytes, below the
    // resolution this reports in.
    return (mDynamicProbeCount * 6 * (mProbeResolution * mProbeResolution) * 4) / 1024 / 1024;
}

GLuint LLReflectionMapManager::allocateQuery()
{
    if (mQueryPool.empty())
    {
        GLuint query;
        glGenQueries(1, &query);
        return query;
    }

    GLuint query = mQueryPool.front();
    mQueryPool.pop_front();
    return query;
}

void LLReflectionMapManager::recycleQuery(GLuint query)
{
    mQueryPool.push_back(query);
}

struct CompareProbeDepth
{
    bool operator()(const LLReflectionMap* lhs, const LLReflectionMap* rhs)
    {
        return lhs->mMinDepth < rhs->mMinDepth;
    }
};

void LLReflectionMapManager::getReflectionMaps(std::vector<LLReflectionMap*>& maps)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

    const LLMatrix4a& modelview = LLViewerCamera::getCurrent().getModelview();
    LLVector4a oa; // scratch space for transformed origin

    // Occlusion is measured from the main camera, so it says nothing about what a probe capture
    // rendering from somewhere else can see. llviewerdisplay stops the queries for the duration
    // of a snapshot for exactly that reason -- "don't read or write it during cube snapshots" --
    // but only the write half was ever honoured, and this is the read. Without it every capture
    // omits whatever the player happens not to be looking at, and bakes that into the probe.
    const bool honor_occlusion = !gCubeSnapshot;

    U32 count = 0;
    U32 lastIdx = 0;
    for (U32 i = 0; count < maps.size() && i < mProbes.size(); ++i)
    {
        // Every probe that does not make the list gets its index cleared, not just the ones
        // without a cube slot. A probe that is occluded or not yet complete used to keep the
        // index it held on some earlier frame, and that slot now belongs to a different probe:
        // the neighbour packer rejects -1 but has no way to tell a stale index from a live one,
        // so a neighbour entry could point at an unrelated probe.
        if (mProbes[i]->mCubeIndex != -1 && mProbes[i]->mComplete &&
            !(honor_occlusion && mProbes[i]->mOccluded))
        {
            maps[count++] = mProbes[i];
            modelview.affineTransform(mProbes[i]->mOrigin, oa);
            mProbes[i]->mMinDepth = -oa.getF32ptr()[2] - mProbes[i]->mRadius;
            mProbes[i]->mMaxDepth = -oa.getF32ptr()[2] + mProbes[i]->mRadius;
        }
        else
        {
            mProbes[i]->mProbeIndex = -1;
        }
        lastIdx = i;
    }

    // set remaining probe indices to -1
    for (U32 i = lastIdx+1; i < mProbes.size(); ++i)
    {
        mProbes[i]->mProbeIndex = -1;
    }

    if (count > 1)
    {
        std::sort(maps.begin(), maps.begin() + count, CompareProbeDepth());
    }

    for (U32 i = 0; i < count; ++i)
    {
        maps[i]->mProbeIndex = i;
    }

    // null terminate list
    if (count < maps.size())
    {
        maps[count] = nullptr;
    }
}

LLReflectionMap* LLReflectionMapManager::registerSpatialGroup(LLSpatialGroup* group)
{
    if (!group)
    {
        return nullptr;
    }
    LLSpatialPartition* part = group->getSpatialPartition();
    if (!part || part->mPartitionType != LLViewerRegion::PARTITION_VOLUME)
    {
        return nullptr;
    }
    OctreeNode* node = group->getOctreeNode();
    F32 size = node->getSize().getF32ptr()[0];
    if (size < 15.f || size > 17.f)
    {
        return nullptr;
    }
    return addProbe(group);
}

LLReflectionMap* LLReflectionMapManager::registerViewerObject(LLViewerObject* vobj)
{
    if (!LLPipeline::sReflectionProbesEnabled)
    {
        return nullptr;
    }

    llassert(vobj != nullptr);

    LLReflectionMap* probe = new LLReflectionMap();
    probe->mViewerObject = vobj;
    // A placed probe, and it is one from the moment it exists. This used to be derived inside
    // autoAdjustOrigin, which is a placement pass and does not run on a probe until it has
    // already won a cube slot -- so a new manual probe carried an automatic probe's priority
    // through the sort that hands slots out, and in a region with none free it could never
    // reach the code that would have told the sort otherwise.
    probe->mPriority = 1;
    probe->mOrigin.load3(vobj->getPositionAgent().mV);

    if (gCubeSnapshot)
    { //snapshot is in progress, mProbes is being iterated over, defer insertion until next update
        mCreateList.push_back(probe);
    }
    else
    {
        mProbes.push_back(probe);
    }

    return probe;
}

// <SK:Nexii> Navmesh rooms probes
LLReflectionMap* LLReflectionMapManager::registerNavRoomsProbe(const LLVector3& origin, F32 radius)
{
    LLReflectionMap* probe = addProbe();
    if (probe)
    {
        probe->mNavRooms = true;
        probe->mOrigin.load3(origin.mV);
        probe->mRadius = radius;
    }
    return probe;
}

LLReflectionMap* LLReflectionMapManager::registerClusterProbe(const LLVector3& origin, F32 radius)
{
    LLReflectionMap* probe = addProbe();
    if (probe)
    {
        probe->mClusterFill = true;
        probe->mOrigin.load3(origin.mV);
        probe->mRadius = radius;
    }
    return probe;
}

std::string LLReflectionMapManager::clusterProbesStatsLine() const
{
    return mClusterProbes->statsLine();
}

void LLReflectionMapManager::updateClusterProbes()
{
    const auto census = SKWorldCensus::instance().snapshot();
    static LLCachedControl<bool> enabled(gSavedSettings, "SKClusterProbes", true);
    if (!enabled || mRenderReflectionProbeLevel < 3)
    {
        mClusterProbes->clear();
        return;
    }
    // The lattice is the census pass's; a scan it has not caught up with yet waits a frame.
    mClusterProbes->update(*this, census, census && census == mSolidsCensus ? mSolids.get() : nullptr);
}

void LLReflectionMapManager::resizeNavRoomsProbe(LLReflectionMap* probe, F32 radius)
{
    probe->mRadius = radius;
    mNavRoomsDirty = true;
    updateNeighbors(probe);
}

void LLReflectionMapManager::staleNavRoomsProbe(LLReflectionMap* probe)
{
    constexpr F32 STALE_AGE_S = 60.f;
    probe->mLastUpdateTime = llmin(probe->mLastUpdateTime, (F32)gFrameTimeSeconds - STALE_AGE_S);
}

void LLReflectionMapManager::invalidateNavRoomsProbe(LLReflectionMap* probe)
{
    if (mUpdatingProbe == probe)
    {
        mUpdatingProbe = nullptr;
        mUpdatingFace = 0;
        mRadiancePass = false;
    }
    probe->mComplete = false;
    probe->mFadeIn = 0.f;
    probe->mOccluded = false;
    mNavRoomsDirty = true;
    updateNeighbors(probe);
}

void LLReflectionMapManager::updateCensusPlacement()
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmu - census placement");
    auto census = SKWorldCensus::instance().snapshot();
    if (mRenderReflectionProbeLevel < 2)
    {
        return;
    }
    // Publish complete immutable indexes; never build or wait in the render loop.
    if (!mSolidWorkers)
    {
        mSolidWorkers = std::make_unique<LL::ThreadPool>("ProbeSolids", 1);
        mSolidWorkers->start();
    }
    auto workers = LL::WorkQueue::getInstance("ProbeSolids");
    if (mSolidJob.valid() && mSolidJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        // The scan the old index described goes with it: this is often its last holder, and
        // freeing a scan's triangle soup took seconds of one frame when it ran here.
        auto retired = std::make_shared<std::pair<std::shared_ptr<sknav::SolidLattice>, std::shared_ptr<const SKCensusSnapshot>>>(
            std::move(mSolids), std::move(mSolidsCensus));
        mSolids = mSolidJob.get();
        mSolidsCensus = std::move(mSolidJobCensus);
        mNavRoomsDirty = true;
        workers->post([retired]() { retired->first.reset(); retired->second.reset(); });
    }
    if (!mSolidJob.valid() && census != mSolidsCensus)
    {
        auto task = std::make_shared<std::packaged_task<std::shared_ptr<sknav::SolidLattice>()>>([census]()
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmu - census lattice worker");
            auto lattice = std::make_shared<sknav::SolidLattice>();
            if (census) lattice->rebuild(census->mRecords);
            return lattice;
        });
        auto future = task->get_future();
        if (workers->tryPost([task]() { (*task)(); }))
        {
            mSolidJob = std::move(future);
            mSolidJobCensus = census;
        }
    }
    // Stamp tests with the snapshot the index actually describes.
    census = mSolidsCensus;
    const bool refresh = mNavRoomsDirty || gFrameTimeSeconds >= mNextCensusPlacement;
    if (refresh)
    {
        mNextCensusPlacement = gFrameTimeSeconds + 1.0;
        mNavRoomsDirty = false;
    }
    const U64 census_stamp = census ? census->mStamp : 0;
    if (!refresh && std::all_of(mProbes.begin(), mProbes.end(), [&](const auto& probe)
        { return probe == mDefaultProbe || probe->mViewerObject || probe->mNavRooms || probe->mClusterFill ||
                 (probe->mGridPlacementChecked && probe->mGridCensusStamp == census_stamp); }))
    {
        return;
    }
    const sknav::ProbeParams params;
    std::vector<sknav::ProbeInfluence> full_probes;

    for (const auto& probe : mProbes)
    {
        if (probe->mViewerObject && !probe->mViewerObject->isDead())
        {
            probe->syncToViewerObject();
            sknav::ProbeInfluence inf;
            inf.mCentre.set(probe->mOrigin.getF32ptr());
            inf.mRadius = probe->mRadius;
            inf.mBox = probe->mViewerObject->getReflectionProbeIsBox();
            if (inf.mBox)
            {
                const auto rot = probe->mViewerObject->getRenderRotation();
                inf.mAxes[0] = LLVector3::x_axis * rot;
                inf.mAxes[1] = LLVector3::y_axis * rot;
                inf.mAxes[2] = LLVector3::z_axis * rot;
                inf.mHalf = probe->mViewerObject->getScale() * 0.5f;
            }
            full_probes.push_back(inf);
        }
        else if (probe->mNavRooms && probe->isRelevant())
        {
            sknav::ProbeInfluence inf;
            inf.mCentre.set(probe->mOrigin.getF32ptr());
            inf.mRadius = probe->mRadius;
            full_probes.push_back(inf);
        }
    }
    // Re-testing a probe against the census can sweep a whole triangle soup,
    // and a new scan marks every grid probe for re-test at once. Bound the
    // time the sweep takes per frame; the rest carries over. mProbes is
    // distance sorted, so the nearest probes are re-tested first.
    constexpr F32 SOLID_CHECK_BUDGET_MS = 2.f;
    const auto solid_checks_started = std::chrono::steady_clock::now();
    for (auto& probe : mProbes)
    {
        if (probe == mDefaultProbe || probe->mViewerObject || probe->mNavRooms || probe->mClusterFill) continue;
        if (!refresh && probe->mGridPlacementChecked && probe->mGridCensusStamp == census_stamp) continue;
        const LLVector3 old(probe->mOrigin.getF32ptr());
        if (!probe->mGridPlacementChecked)
        {
            probe->autoAdjustOrigin();
            probe->mGridOrigin = gAgent.getPosGlobalFromAgent(LLVector3(probe->mOrigin.getF32ptr()));
            probe->mGridRadius = probe->mRadius;
            probe->mGridPlacementChecked = true;
        }
        else if (refresh && !probe->mComplete)
        {
            // A capture still pending on a probe whose surroundings changed: re-nudge
            // toward empty space and re-anchor the demand. Only a refresh reaches this
            // branch: without one, the pass is here solely for probes the census has
            // not answered yet. autoAdjustOrigin re-derives the radius from the octree,
            // so a nudge that settles where it started restores the cleared radius the
            // pass below applies; only a real move re-anchors, and the changed block
            // below then aborts any capture the move tore out from under.
            const LLVector3 placed(probe->mOrigin.getF32ptr());
            const F32 placed_radius = probe->mRadius;
            probe->autoAdjustOrigin();
            if (dist_vec_squared(LLVector3(probe->mOrigin.getF32ptr()), placed) > 0.0001f)
            {
                probe->mGridOrigin = gAgent.getPosGlobalFromAgent(LLVector3(probe->mOrigin.getF32ptr()));
                probe->mGridRadius = probe->mRadius;
                // The solid answer was for where it stood; the move re-opens the question.
                probe->mGridCensusStamp = census_stamp - 1;
            }
            else
            {
                probe->mOrigin.load3(placed.mV);
                probe->mRadius = placed_radius;
            }
        }
        LLVector3 chosen = gAgent.getPosAgentFromGlobal(probe->mGridOrigin);
        F32 radius = probe->mGridRadius;
        bool blocked = probe->mBlockedByCensus;
        if (probe->mGridCensusStamp != census_stamp)
        {
            const F32 elapsed = std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - solid_checks_started).count();
            if (elapsed <= SOLID_CHECK_BUDGET_MS)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmu - census solid test");
                blocked = mSolids->contains(probe->mGridOrigin);
                probe->mGridCensusStamp = census_stamp;
            }
            // Over budget: the stamp stays behind and the test retries next
            // frame, while the radius share below still runs.
        }
        // Full probes have already claimed their space and their slots. The
        // second pass only covers what remains, including high/unwalkable air.
        radius = sknav::clearRadius(chosen, radius, full_probes, params);
        const bool covered = radius <= 0.f;
        const bool changed = dist_vec_squared(old, chosen) > 0.0001f || fabsf(radius - probe->mRadius) > 0.001f ||
                             probe->mBlockedByCensus != blocked || probe->mSuppressedByNavRooms != covered;
        probe->mBlockedByCensus = blocked;
        probe->mSuppressedByNavRooms = covered;
        if (changed)
        {
            // Never finish a capture whose first faces came from another position.
            if (mUpdatingProbe == probe)
            {
                mUpdatingProbe = nullptr;
                mUpdatingFace = 0;
                mRadiancePass = false;
            }
            probe->mOrigin.load3(chosen.mV);
            probe->mRadius = radius;
            probe->mComplete = false;
            probe->mFadeIn = 0.f;
            probe->mOccluded = false;
            updateNeighbors(probe);
        }
    }
}

void LLReflectionMapManager::getNavRoomsContext(std::vector<LLReflectionMap*>& placed) const
{
    placed.clear();
    for (const auto& probe : mProbes)
    {
        if (probe->mViewerObject && !probe->mViewerObject->isDead() && probe->isRelevant())
        {
            placed.push_back(probe);
        }
    }
}
// </SK:Nexii>

// <SK:Nexii> Probe kinds for the census export and the probe volume display.
LLReflectionMapManager::SKProbeKind LLReflectionMapManager::getSKProbeKind(const LLReflectionMap* probe) const
{
    if (probe == mDefaultProbe.get())
    {
        return SK_PROBE_DEFAULT;
    }
    if (probe->mViewerObject)
    {
        return probe->mViewerObject->getReflectionProbeIsBox() ? SK_PROBE_PLACED_BOX : SK_PROBE_PLACED_SPHERE;
    }
    if (probe->mClusterFill)
    {
        return SK_PROBE_CLUSTER;
    }
    return probe->mNavRooms ? SK_PROBE_NAV_SPHERE : SK_PROBE_OCTREE;
}
// </SK:Nexii>

S32 LLReflectionMapManager::allocateCubeIndex()
{
    if (!mCubeFree.empty())
    {
        S32 ret = mCubeFree.front();
        mCubeFree.pop_front();
        return ret;
    }

    return -1;
}

void LLReflectionMapManager::deleteProbe(U32 i)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LLReflectionMap* probe = mProbes[i];

    llassert(probe != mDefaultProbe);

    if (probe->mCubeIndex != -1)
    { // mark the cube index used by this probe as being free
        mCubeFree.push_back(probe->mCubeIndex);
    }
    if (mUpdatingProbe == probe)
    {
        abandonProbeUpdate();
    }

    // remove from any Neighbors lists
    for (auto& other : probe->mNeighbors)
    {
        auto const & iter = std::find(other->mNeighbors.begin(), other->mNeighbors.end(), probe);
        llassert(iter != other->mNeighbors.end());
        other->mNeighbors.erase(iter);
    }

    // Probes are distance sorted, order matters.
    mProbes.erase(mProbes.begin() + i);
}

void LLReflectionMapManager::abandonProbeUpdate()
{
    mUpdatingProbe = nullptr;
    mUpdatingFace = 0;

    // The irradiance half runs first and the radiance half is what marks a probe complete. Left
    // set, this would start the next probe on its radiance half: it would skip the irradiance
    // projection altogether and be marked complete on whatever SH coefficients its slot's
    // previous owner left behind.
    mRadiancePass = false;
}


void LLReflectionMapManager::doProbeUpdate()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(mUpdatingProbe != nullptr);

    updateProbeFace(mUpdatingProbe, mUpdatingFace);

    bool debug_updates = gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_PROBE_UPDATES) && mUpdatingProbe->mViewerObject;

    if (++mUpdatingFace == 6)
    {
        if (debug_updates)
        {
            mUpdatingProbe->mViewerObject->setDebugText(llformat("%.1f", (F32)gFrameTimeSeconds), LLColor4(1, 1, 1, 1));
        }
        updateNeighbors(mUpdatingProbe);
        mUpdatingFace = 0;
        if (isRadiancePass())
        {
            mUpdatingProbe->mComplete = true;
            mNavRoomsDirty = mNavRoomsDirty || mUpdatingProbe->mGroup != nullptr; // <SK:Nexii> Navmesh rooms probes: its origin has settled
            mUpdatingProbe = nullptr;
            mRadiancePass = false;
        }
        else
        {
            mRadiancePass = true;
        }
    }
    else if (debug_updates)
    {
        mUpdatingProbe->mViewerObject->setDebugText(llformat("%.1f", (F32)gFrameTimeSeconds), LLColor4(1, 1, 0, 1));
    }
}

// Do the reflection map update render passes.
// For every 12 calls of this function, one complete reflection probe radiance map and irradiance map is generated
// First six passes render the scene with direct lighting only into a scratch space cube map at the end of the cube map array and generate
// a simple mip chain (not convolution filter).
// At the end of these passes, an irradiance map is generated for this probe and placed into the irradiance cube map array at the index for this probe
// The next six passes render the scene with both radiance and irradiance into the same scratch space cube map and generate a simple mip chain.
// At the end of these passes, a radiance map is generated for this probe and placed into the radiance cube map array at the index for this probe.
// In effect this simulates single-bounce lighting.
void LLReflectionMapManager::updateProbeFace(LLReflectionMap* probe, U32 face)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("probe update");
    // hacky hot-swap of camera specific render targets
    gPipeline.mRT = &gPipeline.mAuxillaryRT;

    mLightScale = 1.f;
    static LLCachedControl<F32> max_local_light_ambiance(gSavedSettings, "RenderReflectionProbeMaxLocalLightAmbiance", 8.f);
    if (!isRadiancePass() && probe->getAmbiance() > max_local_light_ambiance)
    {
        mLightScale = max_local_light_ambiance / probe->getAmbiance();
    }

    if (probe == mDefaultProbe)
    {
        touch_default_probe(probe);

        gPipeline.pushRenderTypeMask();

        //only render sky, water, terrain, and clouds
        gPipeline.andRenderTypeMask(LLPipeline::RENDER_TYPE_SKY, LLPipeline::RENDER_TYPE_WL_SKY,
            LLPipeline::RENDER_TYPE_WATER, LLPipeline::RENDER_TYPE_VOIDWATER, LLPipeline::RENDER_TYPE_CLOUDS, LLPipeline::RENDER_TYPE_TERRAIN, LLPipeline::END_RENDER_TYPES);

        probe->update(mRenderTarget.getWidth(), face);

        gPipeline.popRenderTypeMask();
    }
    else
    {
        llassert(mRenderReflectionProbeLevel > 0); // should never update a probe that's not the default probe if reflection coverage is none
        probe->update(mRenderTarget.getWidth(), face);
    }

    gPipeline.mRT = &gPipeline.mMainRT;

    S32 sourceIdx = mReflectionProbeCount;

    if (probe != mUpdatingProbe)
    { // this is the "realtime" probe that's updating every frame, use the secondary scratch space channel
        sourceIdx += 1;
    }

    gGL.setColorMask(true, true);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);
    LLGLDisable blend(GL_BLEND);

    // downsample to placeholder map
    {
        gGL.matrixMode(gGL.MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();

        gGL.matrixMode(gGL.MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();

        gGL.flush();
        U32 res = mProbeResolution * 2;

        LLRenderTarget* screen_rt = &gPipeline.mAuxillaryRT.screen;

        // perform a gaussian blur on the super sampled render before downsampling
        {
            gGaussianProgram.bind();
            gGaussianProgram.uniform1f(LLShaderMgr::RES_SCALE, 1.f / (mProbeResolution * 2));
            S32 diffuseChannel = gGaussianProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);

            // horizontal
            gGaussianProgram.uniform2f(LLShaderMgr::DIRECTION, 1.f, 0.f);
            gGL.getTextureSlot(diffuseChannel)->bind(screen_rt);
            mRenderTarget.bindTarget();
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            mRenderTarget.flush();

            // vertical
            gGaussianProgram.uniform2f(LLShaderMgr::DIRECTION, 0.f, 1.f);
            gGL.getTextureSlot(diffuseChannel)->bind(&mRenderTarget);
            screen_rt->bindTarget();
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            screen_rt->flush();
        }


        S32 mips = (S32)(log2((F32)mProbeResolution) + 0.5f);

        gReflectionMipProgram.bind();
        S32 diffuseChannel = gReflectionMipProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);

        for (int i = 0; i < mMipChain.size(); ++i)
        {
            LL_PROFILE_GPU_ZONE("probe mip");
            mMipChain[i].bindTarget();
            if (i == 0)
            {
                gGL.getTextureSlot(diffuseChannel)->bind(screen_rt);
            }
            else
            {
                gGL.getTextureSlot(diffuseChannel)->bind(&(mMipChain[i - 1]));
            }


            gReflectionMipProgram.uniform1f(LLShaderMgr::RES_SCALE, 1.f/(mProbeResolution*2));

            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            res /= 2;

            GLint mip = i - (static_cast<GLint>(mMipChain.size()) - mips);

            if (mip >= 0)
            {
                LL_PROFILE_GPU_ZONE("probe mip copy");
                mTexture->bind(0);
                //glCopyTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, mip, 0, 0, probe->mCubeIndex * 6 + face, 0, 0, res, res);
                mTexture->copyFaceFromFramebuffer(mip, sourceIdx, face, res);
                //if (i == 0)
                //{
                    //glCopyTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, mip, 0, 0, probe->mCubeIndex * 6 + face, 0, 0, res, res);
                //}
                mTexture->unbind();
            }
            mMipChain[i].flush();
        }

        gGL.popMatrix();
        gGL.matrixMode(gGL.MM_MODELVIEW);
        gGL.popMatrix();

        gGL.getTextureSlot(diffuseChannel)->unbind();
        gReflectionMipProgram.unbind();
    }

    if (face == 5)
    {
        if (isRadiancePass())
        {
            mMipChain[0].bindTarget();

            //generate radiance map (even if this is not the irradiance map, we need the mip chain for the irradiance map)
            gRadianceGenProgram.bind();
            mVertexBuffer->setBuffer();

            S32 channel = gRadianceGenProgram.enableTexture(LLShaderMgr::REFLECTION_PROBES);
            mTexture->bind(channel);
            gRadianceGenProgram.uniform1i(LLShaderMgr::SOURCE_IDX, sourceIdx);
            gRadianceGenProgram.uniform1f(LLShaderMgr::REFLECTION_PROBE_MAX_LOD, mMaxProbeLOD);
            gRadianceGenProgram.uniform1f(LLShaderMgr::REFLECTION_PROBE_STRENGTH, 1.f);

            U32 res = mMipChain[0].getWidth();

            for (int i = 0; i < mMipChain.size(); ++i)
            {
                LL_PROFILE_GPU_ZONE("probe radiance gen");

                gRadianceGenProgram.uniform1f(LLShaderMgr::ROUGHNESS, (F32)i / (F32)(mMipChain.size() - 1));
                gRadianceGenProgram.uniform1f(LLShaderMgr::MIP_LEVEL, (GLfloat)i);
                gRadianceGenProgram.uniform1i(LLShaderMgr::U_WIDTH, mProbeResolution);

                for (int cf = 0; cf < 6; ++cf)
                { // for each cube face
                    LLCoordFrame frame;
                    frame.lookAt(LLVector3(0, 0, 0), LLCubeMapArray::sClipToCubeLookVecs[cf], LLCubeMapArray::sClipToCubeUpVecs[cf]);

                    F32 mat[16];
                    frame.getOpenGLRotation(mat);
                    gGL.loadMatrix(mat);

                    mVertexBuffer->drawArrays(gGL.TRIANGLE_STRIP, 0, 4);

                    mTexture->copyFaceFromFramebuffer(i, probe->mCubeIndex, cf, res);
                }

                if (i != mMipChain.size() - 1)
                {
                    res /= 2;
                    glViewport(0, 0, res, res);
                }
            }

            gRadianceGenProgram.unbind();
            mMipChain[0].flush();
        }
        else
        {
            LL_PROFILE_GPU_ZONE("probe sh project");

            // Nine coefficients replace the 16x16x6 irradiance cubemap this used to convolve.
            // The old pass re-integrated the environment once per output texel; this integrates
            // it once, into the only nine numbers irradiance actually has.
            //
            // Integrate the mip ALProbeSHProjectionRes asks for, not mip 0. Averaging texels
            // before an integral does not change the integral, and the target basis cannot
            // represent anything the finer mip would add.
            S32 sh_mip = 0;
            const U32 sh_res = shProjectionRes(sh_mip);

            // The last draw is the same for both forms below: nine fragments write this probe's
            // row of mSHCoeffs. This probe owns one row and must not disturb any other, so the
            // viewport is the write mask -- the target is never cleared.
            auto draw_coefficients = [&](LLGLSLShader& program)
            {
                program.uniform1i(LLShaderMgr::U_WIDTH, (S32)sh_res);

                mSHCoeffs.bindTarget();
                glViewport(0, probe->mCubeIndex, LL_SH_COEFF_COUNT, 1);

                mVertexBuffer->setBuffer();
                mVertexBuffer->drawArrays(gGL.TRIANGLE_STRIP, 0, 4);

                mSHCoeffs.flush();
                program.unbind();
            };

            static LLCachedControl<bool> two_pass(gSavedSettings, "ALProbeSHProjectionTwoPass", true);
            const U32 sh_rows = 6 * sh_res;
            if (two_pass && mSHPartial.isComplete() && mSHPartial.getHeight() == sh_rows)
            {
                // Row-parallel form: 9 x 6R fragments each integrate one face row into
                // mSHPartial, then nine fragments add the rows. The single-pass form below
                // serialises the whole cube inside each of nine fragments, which is what made
                // it a multi-millisecond stall however few texels it read.
                {
                    LL_PROFILE_GPU_ZONE("probe sh rows");
                    gSHProjectionRowsProgram.bind();
                    S32 channel = gSHProjectionRowsProgram.enableTexture(LLShaderMgr::REFLECTION_PROBES);
                    mTexture->bind(channel);

                    gSHProjectionRowsProgram.uniform1i(LLShaderMgr::SOURCE_IDX, sourceIdx);
                    gSHProjectionRowsProgram.uniform1f(LLShaderMgr::MIP_LEVEL, (GLfloat)sh_mip);
                    gSHProjectionRowsProgram.uniform1i(LLShaderMgr::U_WIDTH, (S32)sh_res);

                    mSHPartial.bindTarget();
                    // bindTarget sets the viewport to the whole target, which is exactly this
                    // strip; restated because the draw depends on it.
                    glViewport(0, 0, LL_SH_COEFF_COUNT, sh_rows);

                    mVertexBuffer->setBuffer();
                    mVertexBuffer->drawArrays(gGL.TRIANGLE_STRIP, 0, 4);

                    mSHPartial.flush();
                    gSHProjectionRowsProgram.unbind();
                }
                {
                    LL_PROFILE_GPU_ZONE("probe sh reduce");
                    gSHProjectionReduceProgram.bind();
                    // texelFetch source: point sampled, so nothing is filtered across rows
                    gSHProjectionReduceProgram.bindTexture(LLShaderMgr::SH_PARTIAL, &mSHPartial, ALSamplers::PointClamp);
                    draw_coefficients(gSHProjectionReduceProgram);
                }
            }
            else
            {
                // Single pass: nine fragments each walk the whole cube. Kept for A/B captures
                // and as the fallback when the partial target is not there.
                gSHProjectionProgram.bind();
                S32 channel = gSHProjectionProgram.enableTexture(LLShaderMgr::REFLECTION_PROBES);
                mTexture->bind(channel);

                gSHProjectionProgram.uniform1i(LLShaderMgr::SOURCE_IDX, sourceIdx);
                gSHProjectionProgram.uniform1f(LLShaderMgr::MIP_LEVEL, (GLfloat)sh_mip);
                draw_coefficients(gSHProjectionProgram);
            }
        }
    }
}

U32 LLReflectionMapManager::shProjectionRes(S32& mip) const
{
    // Face edge length the SH projection integrates over. Irradiance is band-limited to nine
    // coefficients, so this only has to be fine enough not to alias the source before the
    // integral -- it is not an output resolution and does not bound reconstruction quality.
    // scripts/content_tools/check_sh_projection.py puts the 8x8 default within half a percent
    // of a 128x128 integration; 4x4 is the floor, where that error passes one percent. The
    // cost scales with the texel count, so 32 (the old fixed value) is sixteen times 8.
    static LLCachedControl<S32> projection_res(gSavedSettings, "ALProbeSHProjectionRes", 8);
    const U32 target = llmin((U32)llmax((S32)projection_res, 4), mProbeResolution);

    // Every probe resolution writes mips down to 2x2, so the walk always reaches the target.
    mip = 0;
    U32 res = mProbeResolution;
    while (res > target && (size_t)(mip + 1) < mMipChain.size())
    {
        res >>= 1;
        ++mip;
    }
    return res;
}

void LLReflectionMapManager::reset()
{
    mReset = true;
}

void LLReflectionMapManager::pause(F32 duration)
{
    mPaused = true;
    mResumeTime = gFrameTimeSeconds + duration;
}

void LLReflectionMapManager::resume()
{
    mPaused = false;
}

void LLReflectionMapManager::shift(const LLVector4a& offset)
{
    for (auto& probe : mProbes)
    {
        probe->mOrigin.add(offset);

        // Carried along, because a rigid translation of every probe at once leaves the neighbour
        // graph describing exactly the same geometry. Without this a region crossing would mark
        // all of them stale on the same frame and rebuild the whole graph against itself.
        if (probe->mNeighborRadius >= 0.f)
        {
            probe->mNeighborOrigin.add(offset);
        }
    }
}

void LLReflectionMapManager::updateNeighbors(LLReflectionMap* probe)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

    // Recorded whether or not there is a list to build. The default probe never has neighbours,
    // and without this it would report itself stale on every frame forever.
    probe->mNeighborOrigin = probe->mOrigin;
    probe->mNeighborRadius = probe->mRadius;

    if (mDefaultProbe == probe)
    {
        return;
    }

    //remove from existing neighbors
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmun - clear");

        for (auto& other : probe->mNeighbors)
        {
            auto const & iter = std::find(other->mNeighbors.begin(), other->mNeighbors.end(), probe);
            llassert(iter != other->mNeighbors.end()); // <--- bug davep if this ever happens, something broke badly
            other->mNeighbors.erase(iter);
        }

        probe->mNeighbors.clear();
    }

    probe->mLinkedRelevant = probe->isRelevant(); // <SK:Nexii> Probe links: relevance as this graph saw it

    // search for new neighbors
    if (probe->isRelevant())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmun - search");
        for (auto& other : mProbes)
        {
            if (other != mDefaultProbe && other != probe)
            {
                if (other->isRelevant() && probe->intersects(other))
                {
                    probe->mNeighbors.push_back(other);
                    other->mNeighbors.push_back(probe);
                }
            }
        }
    }
}

// <SK:Nexii> Probe links: every probe whose relevance flipped since the graph last saw it is relinked, all in one sweep.
void LLReflectionMapManager::updateRelevanceLinks()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    std::vector<LLReflectionMap*> flipped;
    for (auto& probe : mProbes)
    {
        if (probe != mDefaultProbe && probe->isRelevant() != probe->mLinkedRelevant)
        {
            flipped.push_back(probe);
        }
    }
    if (flipped.empty())
    {
        return;
    }
    mRelevanceRelinks += static_cast<U32>(flipped.size());
    std::vector<LLReflectionMap*> relevant;
    for (LLReflectionMap* probe : flipped)
    {
        for (auto& other : probe->mNeighbors)
        {
            auto iter = std::find(other->mNeighbors.begin(), other->mNeighbors.end(), probe);
            if (iter != other->mNeighbors.end())
            {
                other->mNeighbors.erase(iter);
            }
        }
        probe->mNeighbors.clear();
        probe->mNeighborOrigin = probe->mOrigin;
        probe->mNeighborRadius = probe->mRadius;
        probe->mLinkedRelevant = probe->isRelevant();
        if (probe->mLinkedRelevant)
        {
            relevant.push_back(probe);
        }
    }
    std::sort(relevant.begin(), relevant.end());
    for (auto& other : mProbes)
    {
        if (other == mDefaultProbe || !other->isRelevant())
        {
            continue;
        }
        const bool other_flipped = std::binary_search(relevant.begin(), relevant.end(), other.get());
        for (LLReflectionMap* probe : relevant)
        {
            // A pair of flipped probes is linked once, from its lower address.
            if (probe == other || (other_flipped && other.get() > probe))
            {
                continue;
            }
            if (probe->intersects(other))
            {
                probe->mNeighbors.push_back(other);
                other->mNeighbors.push_back(probe);
            }
        }
    }
}

U32 LLReflectionMapManager::countNavOctreeLinks() const
{
    U32 links = 0;
    for (const auto& probe : mProbes)
    {
        if (probe->mNavRooms && probe->isRelevant())
        {
            for (const auto& other : probe->mNeighbors)
            {
                links += (other != mDefaultProbe && !other->mViewerObject && !other->mNavRooms && !other->mClusterFill) ? 1 : 0;
            }
        }
    }
    return links;
}
// </SK:Nexii>

void LLReflectionMapManager::updateUniforms()
{
    if (!LLPipeline::sReflectionProbesEnabled)
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("rmmu - uniforms")


    // Have active manual probes live-track the object they're associated with, BEFORE anything
    // derives from their origin or radius.
    //
    // getReflectionMaps takes mMinDepth/mMaxDepth from these values and sorts the array on
    // mMinDepth, and refBucket -- which is the shader's lower bound for its scan -- is only
    // valid while that sort holds. Refreshing afterwards, as this used to, described where the
    // probe was in the bucket and uploaded where it is in refSphere, and a probe whose bucket
    // entry no longer covers it is one the shader scans straight past. Manual probes are the
    // only ones that get this refresh, so they were the only ones that could vanish from it.
    for (auto& probe : mProbes)
    {
        probe->syncToViewerObject();
    }

    mReflectionMaps.resize(mReflectionProbeCount);
    getReflectionMaps(mReflectionMaps);

    F32 minDepth[256];

    for (int i = 0; i < 256; ++i)
    {
        mProbeData.refBucket[i][0] = mReflectionProbeCount;
        mProbeData.refBucket[i][1] = mReflectionProbeCount;
        mProbeData.refBucket[i][2] = mReflectionProbeCount;
        mProbeData.refBucket[i][3] = mReflectionProbeCount;
        minDepth[i] = FLT_MAX;
    }

    // load modelview matrix into matrix 4a
    const LLMatrix4a& modelview = LLViewerCamera::getCurrent().getModelview();
    LLVector4a oa; // scratch space for transformed origin

    S32 count = 0;
    U32 nc = 0; // neighbor "cursor" - index into refNeighbor to start writing the next probe's list of neighbors
    S32 cluster_first = 0, cluster_end = 0; // <SK:Nexii> Cluster probes: the index range the shader scans for them

    LLEnvironment& environment = LLEnvironment::instance();
    LLSettingsSky::ptr_t psky = environment.getCurrentSky();

    static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
    F32 minimum_ambiance = psky->getReflectionProbeAmbiance(should_auto_adjust);

    bool is_ambiance_pass = gCubeSnapshot && !isRadiancePass();
    F32 ambscale = is_ambiance_pass ? 0.f : 1.f;
    ambscale *= mResetFade;
    ambscale = llmax(0, ambscale);
    F32 radscale = is_ambiance_pass ? 0.5f : 1.f;
    radscale *= mResetFade;
    radscale = llmax(0, radscale);

    for (auto* refmap : mReflectionMaps)
    {
        if (refmap == nullptr)
        {
            break;
        }

        if (refmap->mClusterFill) // <SK:Nexii> Cluster probes are never a search's first probe; they span too many neighbours
        {
            cluster_first = cluster_end == 0 ? count : cluster_first;
            cluster_end = count + 1;
        }
        else if (refmap != mDefaultProbe)
        {
            // bucket search data
            // theory of operation:
            //      1. Determine minimum and maximum depth of each influence volume and store in mDepth (done in getReflectionMaps)
            //      2. Sort by minimum depth
            //      3. Prepare a bucket for each 1m of depth out to 256m
            //      4. For each bucket, store the index of the nearest probe that might influence pixels in that bucket
            //      5. In the shader, lookup the bucket for the pixel depth to get the index of the first probe that could possibly influence
            //          the current pixel.
            unsigned int depth_min = llclamp(llfloor(refmap->mMinDepth), 0, 255);
            unsigned int depth_max = llclamp(llfloor(refmap->mMaxDepth), 0, 255);
            for (U32 i = depth_min; i <= depth_max; ++i)
            {
                if (refmap->mMinDepth < minDepth[i])
                {
                    minDepth[i] = refmap->mMinDepth;
                    mProbeData.refBucket[i][0] = refmap->mProbeIndex;
                }
            }
        }

        llassert(refmap->mProbeIndex == count);
        llassert(mReflectionMaps[refmap->mProbeIndex] == refmap);

        llassert(refmap->mCubeIndex >= 0); // should always be  true, if not, getReflectionMaps is bugged

        modelview.affineTransform(refmap->mOrigin, oa);
        mProbeData.refSphere[count].set(oa.getF32ptr());
        mProbeData.refSphere[count].mV[3] = refmap->mRadius;

        mProbeData.refIndex[count][0] = refmap->mCubeIndex;
        llassert(nc % 4 == 0);
        mProbeData.refIndex[count][1] = nc / 4;
        mProbeData.refIndex[count][3] = refmap->mPriority;

        // for objects that are reflection probes, use the volume as the influence volume of the probe
        // only possibile influence volumes are boxes and spheres, so detect boxes and treat everything else as spheres
        if (refmap->getBox(mProbeData.refBox[count]))
        { // negate priority to indicate this probe has a box influence volume
            // Negated from at least 1, because -0 and 0 are the same integer: a box probe that
            // reached here with priority 0 would be read back as an automatic SPHERE, losing
            // both its shape and its precedence. A box influence volume only exists on a probe
            // attached to a viewer object, which is a manual probe by definition.
            mProbeData.refIndex[count][3] = -llmax(mProbeData.refIndex[count][3], 1);
        }

        mProbeData.refParams[count].set(
            llmax(minimum_ambiance, refmap->getAmbiance())*ambscale, // ambiance scale
            radscale, // radiance scale
            refmap->mFadeIn, // fade in weight
            0.f); // unused

        // <SK:Nexii> Probe volume display: the kind rides in bits 24-27 of refParams.w, raw, only while the display is on.
        // Bits 0-15 stay free for the navmesh room box plan; with a kind set the pattern is a normal float, never a denormal.
        // Bit 28 marks a census cluster fill for the shader's tiers (skProbeTier), always.
        static LLCachedControl<bool> render_ref_probe_volumes(gSavedSettings, "RenderReflectionProbeVolumes");
        if (render_ref_probe_volumes || refmap->mClusterFill)
        {
            const U32 bits = (render_ref_probe_volumes ? static_cast<U32>(getSKProbeKind(refmap)) << 24 : 0u) |
                             (refmap->mClusterFill ? 1u << 28 : 0u);
            F32 w;
            memcpy(&w, &bits, sizeof(w));
            mProbeData.refParams[count].mV[3] = w;
        }
        // </SK:Nexii>

        S32 ni = nc; // neighbor ("index") - index into refNeighbor to write indices for current reflection probe's neighbors
        {
            //LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("rmmsu - refNeighbors");
            //pack neghbor list
            const U32 max_neighbors = 64;
            U32 neighbor_count = 0;

            for (auto& neighbor : refmap->mNeighbors)
            {
                if (ni >= 4096)
                { // out of space
                    break;
                }

                // mProbeIndex alone decides this. getReflectionMaps clears it on every probe it
                // leaves out, so it already carries occlusion, completeness and the cube slot,
                // and it is the only one of those that agrees with the list the shader will
                // actually index. Re-testing mOccluded here would disagree with it during a
                // cube snapshot, where occlusion measured from the main camera is ignored:
                // the probe would be in the scan list and unreachable as a neighbour.
                GLint idx = neighbor->mProbeIndex;
                if (idx == -1)
                {
                    continue;
                }

                // this neighbor may be sampled
                mProbeData.refNeighbor[ni++] = idx;

                neighbor_count++;
                if (neighbor_count >= max_neighbors)
                {
                    break;
                }
            }
        }

        if (nc == ni)
        {
            //no neighbors, tag as empty
            mProbeData.refIndex[count][1] = -1;
        }
        else
        {
            mProbeData.refIndex[count][2] = ni - nc;

            // move the cursor forward
            nc = ni;
            if (nc % 4 != 0)
            { // jump to next power of 4 for compatibility with ivec4
                nc += 4 - (nc % 4);
            }
        }


        count++;
    }

#if 0
    {
        // fill in gaps in refBucket
        S32 probe_idx = mReflectionProbeCount;

        for (int i = 0; i < 256; ++i)
        {
            if (i < count)
            { // for debugging, store depth of mReflectionsMaps[i]
                rpd.refBucket[i][1] = (S32) (mReflectionMaps[i]->mDepth * 10);
            }

            if (rpd.refBucket[i][0] == mReflectionProbeCount)
            {
                rpd.refBucket[i][0] = probe_idx;
            }
            else
            {
                probe_idx = rpd.refBucket[i][0];
            }
        }
    }
#endif

    mProbeData.refmapCount = count;
    // <SK:Nexii> Cluster probes: refBucket's .yzw are otherwise unused, and bucket 0 carries their range.
    mProbeData.refBucket[0][1] = cluster_first;
    mProbeData.refBucket[0][2] = cluster_end;
    // </SK:Nexii>

    gPipeline.mHeroProbeManager.updateUniforms();

    // Get the hero data.

    mProbeData.heroBox = gPipeline.mHeroProbeManager.mHeroData.heroBox;
    mProbeData.heroSphere = gPipeline.mHeroProbeManager.mHeroData.heroSphere;
    mProbeData.heroShape  = gPipeline.mHeroProbeManager.mHeroData.heroShape;
    mProbeData.heroMipCount   = gPipeline.mHeroProbeManager.mHeroData.heroMipCount;
    mProbeData.heroProbeCount = gPipeline.mHeroProbeManager.mHeroData.heroProbeCount;

    // SSR march parameters ride along in this block rather than being re-pushed loose on every
    // bindReflectionProbes; they are constant for the frame.
    mProbeData.iterationCount         = (F32)LLPipeline::RenderScreenSpaceReflectionIterations;
    mProbeData.rayStep                = LLPipeline::RenderScreenSpaceReflectionRayStep;
    mProbeData.distanceBias           = LLPipeline::RenderScreenSpaceReflectionDistanceBias;
    mProbeData.depthRejectBias        = LLPipeline::RenderScreenSpaceReflectionDepthRejectBias;
    mProbeData.glossySampleCount      = (F32)LLPipeline::RenderScreenSpaceReflectionGlossySamples;
    mProbeData.adaptiveStepMultiplier = LLPipeline::RenderScreenSpaceReflectionAdaptiveStepMultiplier;

    //copy mProbeData into uniform buffer object
    mUBO.update(&mProbeData, sizeof(ReflectionProbeData));

#if 0
    if (!gCubeSnapshot)
    {
        for (auto& probe : mProbes)
        {
            LLViewerObject* vobj = probe->mViewerObject;
            if (vobj)
            {
                F32 time = (F32)(gFrameTimeSeconds - probe->mLastUpdateTime);
                vobj->setDebugText(llformat("%d/%d/%d/%.1f - %.1f/%.1f", probe->mCubeIndex, probe->mProbeIndex, (U32) probe->mNeighbors.size(), probe->mMinDepth, probe->mMaxDepth, time), time > 1.f ? LLColor4::white : LLColor4::green);
            }
        }
    }
#endif
}

void LLReflectionMapManager::setUniforms()
{
    if (!LLPipeline::sReflectionProbesEnabled)
    {
        return;
    }

    if (!mUBO.allocated())
    {
        updateUniforms();
    }
    mUBO.bind(LLGLSLShader::UB_REFLECTION_PROBES);
}


void renderReflectionProbe(LLReflectionMap* probe)
{
    // <SK:Nexii> Probe links: neighbour links are drawn kind-coloured by LLReflectionMapManager::renderDebug instead.
    if (false && probe->isRelevant()) // </SK:Nexii>
    {
        F32* po = probe->mOrigin.getF32ptr();

        //draw orange line from probe to neighbors
        gGL.flush();
        gGL.diffuseColor4f(1, 0.5f, 0, 1);
        gGL.begin(gGL.LINES);
        for (auto& neighbor : probe->mNeighbors)
        {
            if (probe->mViewerObject && neighbor->mViewerObject)
            {
                continue;
            }

            gGL.vertex3fv(po);
            gGL.vertex3fv(neighbor->mOrigin.getF32ptr());
        }
        gGL.end();
        gGL.flush();

        gGL.diffuseColor4f(1, 1, 0, 1);
        gGL.begin(gGL.LINES);
        for (auto& neighbor : probe->mNeighbors)
        {
            if (probe->mViewerObject && neighbor->mViewerObject)
            {
                gGL.vertex3fv(po);
                gGL.vertex3fv(neighbor->mOrigin.getF32ptr());
            }
        }
        gGL.end();
        gGL.flush();
    }

#if 0
    LLSpatialGroup* group = probe->mGroup;
    if (group)
    { // draw lines from corners of object aabb to reflection probe

        const LLVector4a* bounds = group->getBounds();
        LLVector4a o = bounds[0];

        gGL.flush();
        gGL.diffuseColor4f(0, 0, 1, 1);
        F32* c = o.getF32ptr();

        const F32* bc = bounds[0].getF32ptr();
        const F32* bs = bounds[1].getF32ptr();

        // daaw blue lines from corners to center of node
        gGL.begin(gGL.LINES);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] + bs[0], bc[1] + bs[1], bc[2] + bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] - bs[0], bc[1] + bs[1], bc[2] + bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] + bs[0], bc[1] - bs[1], bc[2] + bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] - bs[0], bc[1] - bs[1], bc[2] + bs[2]);

        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] + bs[0], bc[1] + bs[1], bc[2] - bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] - bs[0], bc[1] + bs[1], bc[2] - bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] + bs[0], bc[1] - bs[1], bc[2] - bs[2]);
        gGL.vertex3fv(c);
        gGL.vertex3f(bc[0] - bs[0], bc[1] - bs[1], bc[2] - bs[2]);
        gGL.end();

        //draw yellow line from center of node to reflection probe origin
        gGL.flush();
        gGL.diffuseColor4f(1, 1, 0, 1);
        gGL.begin(gGL.LINES);
        gGL.vertex3fv(c);
        gGL.vertex3fv(po);
        gGL.end();
        gGL.flush();
    }
#endif
}

namespace
{
    // <SK:Nexii> Probe links: kind colours matching the scan-pulse display in reflectionProbeF.glsl.
    LLColor4 skProbeKindColor(LLReflectionMapManager::SKProbeKind kind)
    {
        switch (kind)
        {
        case LLReflectionMapManager::SK_PROBE_PLACED_SPHERE: return LLColor4(1.f, 0.25f, 0.85f, 1.f);
        case LLReflectionMapManager::SK_PROBE_PLACED_BOX: return LLColor4(1.f, 0.55f, 0.1f, 1.f);
        case LLReflectionMapManager::SK_PROBE_OCTREE: return LLColor4(0.1f, 0.85f, 1.f, 1.f);
        case LLReflectionMapManager::SK_PROBE_NAV_SPHERE: return LLColor4(0.3f, 1.f, 0.35f, 1.f);
        case LLReflectionMapManager::SK_PROBE_NAV_BOX: return LLColor4(1.f, 0.95f, 0.2f, 1.f);
        case LLReflectionMapManager::SK_PROBE_CLUSTER: return LLColor4(0.55f, 0.45f, 1.f, 1.f);
        default: return LLColor4(0.7f, 0.7f, 0.7f, 1.f);
        }
    }

    // Probe link weights: one probe's influence at a world point, for drawing a link as the shader blends it.
    struct SKProbeLinkEnd
    {
        LLReflectionMap* probe;
        LLMatrix4a toUnit; // world to unit box, box probes only
        bool box;
        bool manual;
        S32 tier; // skProbeTier in reflectionProbeF.glsl: 0 automatic, 1 manual, 2 cluster fill, 3 default
    };

    SKProbeLinkEnd skProbeLinkEnd(LLReflectionMap* probe)
    {
        SKProbeLinkEnd e{ probe, LLMatrix4a(), false, probe->mPriority > 0, 0 };
        LLViewerObject* obj = probe->mViewerObject;
        if (obj && obj->getReflectionProbeIsBox() && obj->getVolume() && obj->mDrawable.notNull())
        {
            LLVector3 s = obj->getScale() * 0.5f;
            e.toUnit.setMul(LLMatrix4a::scaling(s.mV[0], s.mV[1], s.mV[2]), obj->mDrawable->getWorldMatrix());
            e.toUnit.invert();
            e.box = true;
            e.manual = true;
        }
        e.tier = e.manual ? 1 : probe->mClusterFill ? 2 : 0; // the default probe is never linked
        return e;
    }

    // Mirrors skProbeRawWeight/sphereWeight/boxDistanceWeight and the in-volume test of shouldSampleProbe in
    // class3/deferred/reflectionProbeF.glsl (rp:80-117, rp:481-509, rp:963-973); w = 0 outside the volume.
    F32 skProbeLinkWeight(const SKProbeLinkEnd& e, const LLVector3& pos, F32& dw, bool& insideBox)
    {
        dw = 0.f;
        const F32 fade = e.probe->mFadeIn;
        if (e.box)
        {
            LLVector4a p, u;
            p.load3(pos.mV);
            e.toUnit.affineTransform(p, u);
            const F32 m = llmax(fabsf(u[0]), llmax(fabsf(u[1]), fabsf(u[2])));
            if (m > 1.f)
            {
                return 0.f;
            }
            insideBox = true;
            const F32 d = 1.f - m;
            dw = llclamp(d, 0.f, 1.f) * fade;
            return llmax(d, 0.001f);
        }
        const F32 r = e.probe->mRadius;
        const F32 d2 = llmax((pos - LLVector3(e.probe->mOrigin.getF32ptr())).magVec(), 0.001f);
        if (d2 > r)
        {
            return 0.f;
        }
        const F32 r1 = r * 0.5f;
        const F32 atten = 1.f - llmax(d2 - r1, 0.f) / llmax(r - r1, 0.001f);
        const F32 w = fade / d2;
        dw = w * atten * llmax(r, 1.f) * 4.f;
        return w * atten;
    }

    // Both probes' normalised shares at pos, tiers mixed as skMixProbeTiers does in reflectionProbeF.glsl.
    void skProbeLinkShares(const SKProbeLinkEnd& a, const SKProbeLinkEnd& b, const LLVector3& pos, F32& sa, F32& sb)
    {
        bool insideBox = false;
        F32 dwa, dwb;
        F32 wa = skProbeLinkWeight(a, pos, dwa, insideBox);
        F32 wb = skProbeLinkWeight(b, pos, dwb, insideBox);
        if (insideBox)
        {
            if (!a.manual) wa = 0.f;
            if (!b.manual) wb = 0.f;
        }
        F32 wsum[4] = { 0.f, 0.f, 0.f, 0.f };
        F32 dwsum[4] = { 0.f, 0.f, 0.f, 0.f };
        wsum[a.tier] += wa;
        dwsum[a.tier] += dwa;
        wsum[b.tier] += wb;
        dwsum[b.tier] += dwb;
        // Top down, each present tier takes its distance weight of what is left, all of it when nothing lies below.
        constexpr S32 TOP_DOWN[4] = { 1, 0, 2, 3 };
        F32 tierShare[4] = { 0.f, 0.f, 0.f, 0.f };
        F32 remaining = 1.f;
        for (S32 k = 0; k < 4; ++k)
        {
            const S32 t = TOP_DOWN[k];
            bool lower = false;
            for (S32 l = k + 1; l < 4; ++l)
            {
                lower = lower || wsum[TOP_DOWN[l]] > 0.f;
            }
            tierShare[t] = wsum[t] > 0.f ? remaining * (lower ? llmin(dwsum[t], 1.f) : 1.f) : 0.f;
            remaining -= tierShare[t];
        }
        sa = wsum[a.tier] > 0.f ? wa / wsum[a.tier] * tierShare[a.tier] : 0.f;
        sb = wsum[b.tier] > 0.f ? wb / wsum[b.tier] * tierShare[b.tier] : 0.f;
    }
    // </SK:Nexii>
}

void LLReflectionMapManager::renderDebug()
{
    gDebugProgram.bind();

    for (auto& probe : mProbes)
    {
        renderReflectionProbe(probe);
    }

    // <SK:Nexii> Probe links: each neighbour pair once, coloured and dashed by the shader's blend weights along it.
    std::vector<std::pair<LLColor4, std::vector<LLVector3>>> buckets; // gDebugProgram colours by uniform only
    for (auto& probe : mProbes)
    {
        if (!probe->isRelevant())
        {
            continue;
        }
        const LLColor4 ca = skProbeKindColor(getSKProbeKind(probe));
        const LLVector3 pa(probe->mOrigin.getF32ptr());
        for (auto& neighbor : probe->mNeighbors)
        {
            if (neighbor->isRelevant() && neighbor < probe.get())
            {
                continue;
            }
            const LLColor4 cb = skProbeKindColor(getSKProbeKind(neighbor));
            const LLVector3 pb(neighbor->mOrigin.getF32ptr());
            const F32 len = (pb - pa).magVec();
            if (len < 1e-3f)
            {
                continue;
            }
            const SKProbeLinkEnd ea = skProbeLinkEnd(probe);
            const SKProbeLinkEnd eb = skProbeLinkEnd(neighbor);
            auto seg = [&](const LLColor4& c, F32 m0, F32 m1)
            {
                auto it = std::find_if(buckets.begin(), buckets.end(), [&](const auto& e) { return e.first == c; });
                if (it == buckets.end())
                {
                    it = buckets.emplace(buckets.end(), c, std::vector<LLVector3>());
                }
                it->second.push_back(lerp(pa, pb, m0 / len));
                it->second.push_back(lerp(pa, pb, m1 / len));
            };
            // Solid in the dominant colour at >= 90% share, else dashes from 0.05 m (even) to 0.5 m (near 90%).
            constexpr F32 STEP = 0.05f, DASH_MIN = 0.05f, DASH_MAX = 0.5f, SOLID = 0.9f;
            auto blendAt = [&](F32 m, F32& w1, F32& w2, bool& aFirst)
            {
                F32 sa, sb;
                skProbeLinkShares(ea, eb, lerp(pa, pb, m / len), sa, sb);
                aFirst = sa >= sb;
                w1 = llmax(sa, sb);
                w2 = llmin(sa, sb);
            };
            S32 dash = 0;
            F32 m = 0.f;
            for (S32 n = 0; m < len && n < 20000; ++n)
            {
                F32 w1, w2;
                bool aFirst;
                blendAt(m + llmin(STEP, len - m) * 0.5f, w1, w2, aFirst);
                const LLColor4& dom = aFirst ? ca : cb;
                if (w1 <= 0.f)
                {
                    const F32 e = llmin(m + STEP, len);
                    seg(LLColor4(0.4f, 0.4f, 0.4f, 1.f), m, e); // neither probe reaches here
                    m = e;
                    dash = 0;
                    continue;
                }
                if (w1 >= SOLID || w2 <= 0.f)
                {
                    const F32 e = llmin(m + STEP, len);
                    seg(dom, m, e);
                    m = e;
                    dash = 0;
                    continue;
                }
                const F32 evenness = llclamp((w2 / w1 - (1.f - SOLID) / SOLID) / (1.f - (1.f - SOLID) / SOLID), 0.f, 1.f);
                const F32 dashLen = lerp(DASH_MAX, DASH_MIN, evenness);
                F32 ew1, ew2;
                bool eaFirst;
                if (m + dashLen > len || (blendAt(m + dashLen, ew1, ew2, eaFirst), ew1 >= SOLID || ew2 <= 0.f))
                {
                    const F32 e = llmin(m + STEP, len); // no room for a whole dash
                    seg(dom, m, e);
                    m = e;
                    continue;
                }
                seg((dash++ & 1) ? cb : ca, m, m + dashLen);
                m += dashLen;
            }
        }
    }
    for (const auto& bucket : buckets)
    {
        gGL.diffuseColor4fv(bucket.first.mV);
        gGL.begin(LLRender::LINES);
        for (const LLVector3& v : bucket.second)
        {
            gGL.vertex3fv(v.mV);
        }
        gGL.end();
        gGL.flush();
    }
    // </SK:Nexii>

    for (const auto& probe : mProbes)
    {
        // Markers share the scan-pulse kind colours; a census-blocked probe stays red.
        const LLColor4 color = probe->mBlockedByCensus && probe != mDefaultProbe ? LLColor4(1.f, 0.2f, 0.2f, 1.f) : skProbeKindColor(getSKProbeKind(probe));
        const LLVector3 origin(probe->mOrigin.getF32ptr());
        gGL.diffuseColor4fv(color.mV);
        gGL.begin(LLRender::LINES);
        for (S32 axis = 0; axis < 3; ++axis)
        {
            LLVector3 offset;
            offset.mV[axis] = 0.5f;
            gGL.vertex3fv((origin - offset).mV);
            gGL.vertex3fv((origin + offset).mV);
        }
        gGL.end();
        gGL.flush();
    }

    gDebugProgram.unbind();
}

void LLReflectionMapManager::initReflectionMaps()
{
    static LLCachedControl<U32> ref_probe_res(gSavedSettings, "RenderReflectionProbeResolution", 128U);
    U32 probe_resolution = nhpo2(llclamp(ref_probe_res(), (U32)64, (U32)512));

    // The supersample factor sizes the scratch target and, through LLPipeline, the aux pack a
    // face renders into; a change is applied like a resolution change, by the listener that
    // rebuilds the GL buffers (handleReflectionProbeDetailChanged), so here it only has to be
    // read. 1, 2 or 4: the blur and downsample below assume a power of two.
    static LLCachedControl<S32> super_sample(gSavedSettings, "ALProbeSuperSample", 4);
    const U32 supersample = super_sample >= 4 ? 4u : (super_sample >= 2 ? 2u : 1u);
    if (supersample != mSuperSample)
    {
        mSuperSample = supersample;
        mRenderTarget.release();
    }
    // No irradiance resolution to size any more: irradiance is nine SH coefficients whatever
    // the environment looks like, so the setting that used to pick a cubemap edge length is gone.
    if (mTexture.isNull() || mReflectionProbeCount != mDynamicProbeCount || mProbeResolution != probe_resolution ||
        !mSHCoeffs.isComplete() || mReset)
    {
        if(mProbeResolution != probe_resolution)
        {
            mRenderTarget.release();
            mMipChain.clear();
        }

        gEXRImage = nullptr;
        mReset = false;
        mReflectionProbeCount = mDynamicProbeCount;
        mProbeResolution = probe_resolution;
        mMaxProbeLOD = log2f((F32)mProbeResolution) - 1.f; // number of mips - 1

        // Signed storage: the linear and quadratic bands are negative over half the sphere, so
        // none of the unsigned float formats the radiance chain uses can hold these.
        mSHCoeffs.release();
        mSHCoeffs.allocate(LL_SH_COEFF_COUNT, mReflectionProbeCount + 2, GL_RGBA16F);

        if (mTexture.isNull() ||
            mTexture->getWidth() != mProbeResolution ||
            mReflectionProbeCount + 2 != mTexture->getCount())
        {
#if 0 // Cubemap copy critically flawed and overflows
            if (mTexture)
            {
                mTexture = new LLCubeMapArray(*mTexture, mProbeResolution, mReflectionProbeCount + 2);
            }
            else
#endif
            {
                mTexture = new LLCubeMapArray();

                static LLCachedControl<bool> render_hdr(gSavedSettings, "RenderHDREnabled", true);

                // store mReflectionProbeCount+2 cube maps, final two cube maps are used for render target and radiance map generation
                // source)
                mTexture->allocate(mProbeResolution, 3, mReflectionProbeCount + 2, true, render_hdr);
            }
        }

        // reset probe state
        mUpdatingFace = 0;
        mUpdatingProbe = nullptr;
        mRadiancePass = false;
        mRealtimeRadiancePass = false;

        // if default probe already exists, remember whether or not it's complete (SL-20498)
        bool default_complete = mDefaultProbe.isNull() ? false : mDefaultProbe->mComplete;

        for (auto& probe : mProbes)
        {
            probe->mLastUpdateTime = 0.f;
            probe->mComplete = false;
            probe->mProbeIndex = -1;
            probe->mCubeArray = nullptr;
            probe->mCubeIndex = -1;
            probe->mNeighbors.clear();
            probe->mNeighborRadius = -1.f; // list cleared, so it describes nothing
            probe->mFadeIn = 0;
            // Both of these decide whether a probe is allowed to render, so a reset that left
            // them standing could not clear a probe that was stuck on either -- and this is the
            // path a teleport, a sky change or a probe-count change goes through, which is
            // exactly where someone would expect to get their probes back.
            probe->mOccluded = false;
            probe->mInsideManualProbe = false;
        }

        mCubeFree.clear();
        initCubeFree();

        if (mDefaultProbe.isNull())
        {
            llassert(mProbes.empty()); // default probe MUST be the first probe created
            mDefaultProbe = new LLReflectionMap();
            mProbes.push_back(mDefaultProbe);
        }

        llassert(mProbes[0] == mDefaultProbe);

        mDefaultProbe->mCubeIndex = 0;
        mDefaultProbe->mCubeArray = mTexture;
        mDefaultProbe->mDistance = 64.f;
        mDefaultProbe->mRadius = 4096.f;
        mDefaultProbe->mProbeIndex = 0;
        mDefaultProbe->mComplete = default_complete;

        touch_default_probe(mDefaultProbe);
    }

    if (mVertexBuffer.isNull())
    {
        U32 mask = LLVertexBuffer::MAP_VERTEX;
        LLPointer<LLVertexBuffer> buff = new LLVertexBuffer(mask);
        buff->allocateBuffer(4, 0);

        LLStrider<LLVector3> v;

        buff->getVertexStrider(v);

        // NOTE: z=-1 is load-bearing. radianceGenV/irradianceGenV use this position for BOTH
        // the clip-space gl_Position AND (rotated by modelview) the cubemap SAMPLE DIRECTION,
        // so z is the forward axis of the convolution -- do NOT flatten it. The clip-volume
        // conflict under reverse-Z (z=-1 is outside ZERO_TO_ONE) is resolved in the shaders,
        // which remap only the output clip z while keeping the direction from this raw z=-1.
        v[0] = LLVector3(-1, -1, -1);
        v[1] = LLVector3(1, -1, -1);
        v[2] = LLVector3(-1, 1, -1);
        v[3] = LLVector3(1, 1, -1);

        buff->unmapBuffer();

        mVertexBuffer = buff;
    }
}

void LLReflectionMapManager::cleanup()
{
    mVertexBuffer = nullptr;
    mRenderTarget.release();

    mMipChain.clear();

    mTexture = nullptr;
    mSHCoeffs.release();
    mSHPartial.release();

    mProbes.clear();
    mKillList.clear();
    mCreateList.clear();

    mReflectionMaps.clear();
    abandonProbeUpdate();

    // <SK:Nexii> Navmesh rooms probes: join the solids pool here, as gPipeline is destroyed after the fiber machinery.
    if (mSolidWorkers)
    {
        mSolidWorkers->close();
        mSolidWorkers.reset();
    }
    // </SK:Nexii>

    mDefaultProbe = nullptr;

    mUBO.release();

    cleanupQueryPool();

    // note: also called on teleport (not just shutdown), so make sure we're in a good "starting" state
    initCubeFree();
}

void LLReflectionMapManager::cleanupQueryPool()
{
    if (!mQueryPool.empty())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("cleanup query pool");
        std::vector<GLuint> queries(mQueryPool.begin(), mQueryPool.end());
        glDeleteQueries(static_cast<GLsizei>(queries.size()), queries.data());
        mQueryPool.clear();
    }
}

void LLReflectionMapManager::doOcclusion()
{
    LLVector4a eye;
    eye.load3(LLViewerCamera::instance().getOrigin().mV);

    for (auto& probe : mProbes)
    {
        if (probe.isNull() || probe == mDefaultProbe)
        {
            continue;
        }

        // A probe holding no cube data, or one relevance has switched off, cannot reach the
        // frame whatever a query says about it -- and each query costs a box draw, so this was
        // up to a couple of hundred of them per frame spent on probes with nothing to show.
        // Cleared rather than left to go stale: a probe that becomes eligible again should be
        // culled by a query taken while it had something to show, not by one taken before.
        if (probe->mCubeIndex == -1 || !probe->isRelevant())
        {
            probe->mOccluded = false;
            continue;
        }

        probe->doOcclusion(eye);
    }
}

void LLReflectionMapManager::forceDefaultProbeAndUpdateUniforms(bool force)
{
    // Saved against the probe itself rather than against its position in mProbes. The list can
    // gain or lose entries between the two calls -- the create and kill lists are drained in
    // update(), and a probe can be dropped the moment nothing outside the manager holds it --
    // and an index-keyed restore then hands one probe's occlusion state to another. The
    // LLPointer also keeps every saved probe alive until the restore.
    static std::vector<std::pair<LLPointer<LLReflectionMap>, bool> > sSavedOcclusion;

    if (force)
    {
        llassert(sSavedOcclusion.empty());

        for (auto& probe : mProbes)
        {
            sSavedOcclusion.emplace_back(probe, probe->mOccluded);
            if (probe != mDefaultProbe)
            {
                probe->mOccluded = true;
            }
        }

        updateUniforms();
    }
    else
    {
        for (auto& saved : sSavedOcclusion)
        {
            saved.first->mOccluded = saved.second;
        }
        sSavedOcclusion.clear();
        sSavedOcclusion.shrink_to_fit();
    }
}
