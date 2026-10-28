/**
 * @file skinventoryexplorerassets.cpp
 * @brief Inventory Explorer's asset reads for the details: sounds, animations, experiences and landmarks
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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

#include "skinventoryexplorerassets.h"

#include "llagent.h"
#include "llassetstorage.h"
#include "lldatapacker.h"
#include "llexperiencecache.h"
#include "llfilesystem.h"
#include "llinventorymodel.h"
#include "llkeyframemotion.h"
#include "lllandmarklist.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "workqueue.h"

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#ifndef POCKETFFT_NO_MULTITHREADING
#define POCKETFFT_NO_MULTITHREADING
#endif
#include <pocketfft_hdronly.h>

#include <complex>
#include <map>
#include <memory>

namespace
{
    U32 sRevision = 1;
    std::map<LLUUID, SKSoundInfo> sSounds;
    std::map<LLUUID, SKAnimInfo> sAnimations;
    std::map<LLUUID, SKExperienceInfo> sExperiences;

    // Band split points, to tune on real sounds.
    constexpr F32 BASS_HZ = 250.f;
    constexpr F32 TREBLE_HZ = 4000.f;
    constexpr size_t MAX_FFT = 8192;
    constexpr size_t FFT_WINDOWS = 3;       // spread across each slice, averaged
    constexpr S64 MAX_SECONDS = 30;         // a sound asset's own limit; nothing past it is decoded

    bool read_cached_asset(const LLUUID& id, LLAssetType::EType type, std::vector<U8>& data)
    {
        LLFileSystem file(id, type, LLFileSystem::READ);
        const S32 size = file.getSize();
        if (size <= 0)
        {
            return false;
        }
        data.resize(size);
        return file.read(data.data(), size);
    }

    // Ogg read straight from memory.
    struct MemoryOgg
    {
        const std::vector<U8>* mData;
        size_t mPos;
    };

    size_t ogg_read(void* ptr, size_t size, size_t nmemb, void* source)
    {
        MemoryOgg* ogg = static_cast<MemoryOgg*>(source);
        const size_t left = ogg->mData->size() - ogg->mPos;
        const size_t bytes = llmin(left, size * nmemb);
        memcpy(ptr, ogg->mData->data() + ogg->mPos, bytes);
        ogg->mPos += bytes;
        return size ? bytes / size : 0;
    }

    int ogg_seek(void* source, ogg_int64_t offset, int whence)
    {
        MemoryOgg* ogg = static_cast<MemoryOgg*>(source);
        S64 pos = whence == SEEK_SET ? offset : whence == SEEK_CUR ? (S64)ogg->mPos + offset : (S64)ogg->mData->size() + offset;
        if (pos < 0 || pos > (S64)ogg->mData->size())
        {
            return -1;
        }
        ogg->mPos = (size_t)pos;
        return 0;
    }

    long ogg_tell(void* source)
    {
        return (long)static_cast<MemoryOgg*>(source)->mPos;
    }

    // Runs on a worker: the whole decode, then a peak and an FFT band split per slice.
    std::shared_ptr<SKSoundInfo> decode_sound_unguarded(const std::vector<U8>& data)
    {
        auto info = std::make_shared<SKSoundInfo>();
        info->mState = SK_ASSET_FAILED;
        MemoryOgg source{ &data, 0 };
        ov_callbacks callbacks = { ogg_read, ogg_seek, nullptr, ogg_tell };
        OggVorbis_File vf;
        if (ov_open_callbacks(&source, &vf, nullptr, 0, callbacks) < 0)
        {
            return info;
        }
        const vorbis_info* vi = ov_info(&vf, -1);
        if (!vi || vi->channels < 1 || vi->rate < 1)
        {
            ov_clear(&vf);
            return info;
        }
        info->mRate = (S32)vi->rate;
        info->mChannels = vi->channels;
        // The stream's own length is not trusted: the decode stops at the cap.
        const size_t cap = (size_t)llclamp((S64)vi->rate, (S64)1, (S64)192000) * (size_t)MAX_SECONDS;
        std::vector<float> mono;
        float** pcm = nullptr;
        int section = 0;
        long frames = 0;
        while (mono.size() < cap && (frames = ov_read_float(&vf, &pcm, 4096, &section)) > 0)
        {
            frames = (long)llmin((size_t)frames, cap - mono.size());
            for (long i = 0; i < frames; ++i)
            {
                float sum = 0.f;
                for (int c = 0; c < vi->channels; ++c)
                {
                    sum += pcm[c][i];
                }
                mono.push_back(sum / (float)vi->channels);
            }
        }
        ov_clear(&vf);
        if (mono.empty())
        {
            return info;
        }

        info->mLength = (F32)mono.size() / (F32)info->mRate;
        const size_t slices = SKSoundInfo::SLICES;
        info->mPeaks.assign(slices, 0.f);
        info->mBands.assign(slices, LLVector3(1.f / 3.f, 1.f / 3.f, 1.f / 3.f));
        const size_t slice_len = llmax((size_t)1, (mono.size() + slices - 1) / slices);
        std::vector<float> window;
        std::vector<std::complex<float>> spectrum;
        F32 loudest = 0.f;
        for (size_t s = 0; s < slices; ++s)
        {
            const size_t begin = s * slice_len;
            if (begin >= mono.size())
            {
                break;
            }
            const size_t end = llmin(mono.size(), begin + slice_len);
            F32 peak = 0.f;
            for (size_t i = begin; i < end; ++i)
            {
                peak = llmax(peak, fabsf(mono[i]));
            }
            info->mPeaks[s] = peak;
            loudest = llmax(loudest, peak);

            // Hann-windowed FFTs spread across the slice, their energy summed per band.
            const size_t length = end - begin;
            const size_t n = llmin(length, MAX_FFT);
            if (n < 16)
            {
                continue;
            }
            const size_t windows = length > n ? FFT_WINDOWS : 1;
            F32 bands[3] = { 0.f, 0.f, 0.f };
            window.resize(n);
            spectrum.resize(n / 2 + 1);
            for (size_t wi = 0; wi < windows; ++wi)
            {
                const size_t start = begin + (windows > 1 ? wi * (length - n) / (windows - 1) : 0);
                for (size_t i = 0; i < n; ++i)
                {
                    const float w = 0.5f - 0.5f * cosf(F_TWO_PI * (float)i / (float)(n - 1));
                    window[i] = mono[start + i] * w;
                }
                pocketfft::shape_t shape{ n };
                pocketfft::stride_t stride_in{ (ptrdiff_t)sizeof(float) };
                pocketfft::stride_t stride_out{ (ptrdiff_t)sizeof(std::complex<float>) };
                pocketfft::r2c(shape, stride_in, stride_out, 0, pocketfft::FORWARD, window.data(), spectrum.data(), 1.f);
                for (size_t k = 1; k < spectrum.size(); ++k)
                {
                    const F32 hz = (F32)k * (F32)info->mRate / (F32)n;
                    bands[hz < BASS_HZ ? 0 : hz < TREBLE_HZ ? 1 : 2] += std::norm(spectrum[k]);
                }
            }
            const F32 sum = bands[0] + bands[1] + bands[2];
            if (sum > 0.f)
            {
                info->mBands[s].set(bands[0] / sum, bands[1] / sum, bands[2] / sum);
            }
        }
        if (loudest > 0.f)
        {
            for (F32& peak : info->mPeaks)
            {
                peak /= loudest;
            }
        }
        info->mState = SK_ASSET_READY;
        return info;
    }

    // A corrupt or hostile asset fails alone; the reply is always posted.
    std::shared_ptr<SKSoundInfo> decode_sound(const std::vector<U8>& data)
    {
        try
        {
            return decode_sound_unguarded(data);
        }
        catch (const std::exception& e)
        {
            LL_WARNS("InventoryExplorer") << "A sound decode failed: " << e.what() << LL_ENDL;
        }
        catch (...)
        {
            LL_WARNS("InventoryExplorer") << "A sound decode failed" << LL_ENDL;
        }
        auto info = std::make_shared<SKSoundInfo>();
        info->mState = SK_ASSET_FAILED;
        return info;
    }

    void on_sound_fetched(const LLUUID& id, S32 status)
    {
        auto it = sSounds.find(id);
        if (it == sSounds.end())
        {
            return;
        }
        auto data = std::make_shared<std::vector<U8>>();
        LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
        LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
        if (status != LL_ERR_NOERR || !read_cached_asset(id, LLAssetType::AT_SOUND, *data) || !main_queue || !general_queue
            || !main_queue->postTo(general_queue, [data]() { return decode_sound(*data); },
                                   [id](std::shared_ptr<SKSoundInfo> info)
                                   {
                                       sSounds[id] = *info;
                                       ++sRevision;
                                   }))
        {
            it->second.mState = SK_ASSET_FAILED;
            ++sRevision;
        }
    }

    // Exposes the joint motion list the deserializer fills.
    class SKAnimReader : public LLKeyframeMotion
    {
    public:
        SKAnimReader(const LLUUID& id) : LLKeyframeMotion(id) {}
        JointMotionList* list() { return mJointMotionList; }
    };

    const char* joint_region(const std::string& name)
    {
        if (name.rfind("mHand", 0) == 0)
        {
            return "hands";
        }
        if (name.rfind("mFace", 0) == 0)
        {
            return "face";
        }
        if (name.rfind("mWing", 0) == 0)
        {
            return "wings";
        }
        if (name.rfind("mTail", 0) == 0)
        {
            return "tail";
        }
        if (name.rfind("mHind", 0) == 0)
        {
            return "hind";
        }
        return "body";
    }

    void read_motion_list(LLKeyframeMotion::JointMotionList* list, SKAnimInfo& info)
    {
        info.mDuration = list->mDuration;
        info.mLoop = list->mLoop;
        info.mLoopIn = list->mLoopInPoint;
        info.mLoopOut = list->mLoopOutPoint;
        info.mEaseIn = list->mEaseInDuration;
        info.mEaseOut = list->mEaseOutDuration;
        info.mPriority = (S32)list->mBasePriority;
        info.mHandPose = (S32)list->mHandPose;
        info.mEmote = list->mEmoteName;
        info.mJoints = (S32)list->getNumJointMotions();
        info.mRegions.clear();
        for (const char* region : { "body", "hands", "face", "wings", "tail", "hind" })
        {
            for (U32 i = 0; i < list->getNumJointMotions(); ++i)
            {
                if (!strcmp(joint_region(list->getJointMotion(i)->mJointName), region))
                {
                    info.mRegions.push_back(region);
                    break;
                }
            }
        }
        info.mState = SK_ASSET_READY;
    }

    void on_animation_fetched(const LLUUID& id, S32 status)
    {
        auto it = sAnimations.find(id);
        if (it == sAnimations.end())
        {
            return;
        }
        ++sRevision;
        std::vector<U8> data;
        if (status != LL_ERR_NOERR || !isAgentAvatarValid() || !read_cached_asset(id, LLAssetType::AT_ANIMATION, data))
        {
            it->second.mState = SK_ASSET_FAILED;
            return;
        }
        // The deserializer resolves joints on a character: our own avatar, as the preview does.
        auto reader = std::make_unique<SKAnimReader>(id);
        reader->setCharacter(gAgentAvatarp);
        LLDataPackerBinaryBuffer dp(data.data(), (S32)data.size());
        const bool was_cached = LLKeyframeDataCache::getKeyframeData(id) != nullptr;
        if (reader->deserialize(dp, id, true) && reader->list())
        {
            read_motion_list(reader->list(), it->second);
        }
        else
        {
            it->second.mState = SK_ASSET_FAILED;
        }
        // Read for its rows only: the cache is left as it was.
        reader.reset();
        if (!was_cached)
        {
            LLKeyframeDataCache::removeKeyframeData(id);
        }
    }
}

namespace SKInventoryAssets
{
    U32 revision()
    {
        return sRevision;
    }

    const SKSoundInfo* sound(const LLUUID& asset_id, bool fetch)
    {
        auto it = sSounds.find(asset_id);
        if (it != sSounds.end())
        {
            return &it->second;
        }
        if (!fetch || asset_id.isNull() || !gAssetStorage)
        {
            return nullptr;
        }
        SKSoundInfo& info = sSounds[asset_id];
        gAssetStorage->getAssetData(asset_id, LLAssetType::AT_SOUND,
                                    [](const LLUUID& id, LLAssetType::EType, void*, S32 status, LLExtStat) { on_sound_fetched(id, status); },
                                    nullptr, true);
        return &info;
    }

    const SKAnimInfo* animation(const LLUUID& asset_id, bool fetch)
    {
        auto it = sAnimations.find(asset_id);
        if (it != sAnimations.end())
        {
            return &it->second;
        }
        if (!fetch || asset_id.isNull() || !gAssetStorage)
        {
            return nullptr;
        }
        SKAnimInfo& info = sAnimations[asset_id];
        // An animation any character has played is read from the keyframe cache, with no fetch.
        if (LLKeyframeMotion::JointMotionList* list = LLKeyframeDataCache::getKeyframeData(asset_id))
        {
            read_motion_list(list, info);
            return &info;
        }
        gAssetStorage->getAssetData(asset_id, LLAssetType::AT_ANIMATION,
                                    [](const LLUUID& id, LLAssetType::EType, void*, S32 status, LLExtStat) { on_animation_fetched(id, status); },
                                    nullptr, true);
        return &info;
    }

    const SKExperienceInfo* experience(const LLUUID& item_id, bool fetch)
    {
        auto it = sExperiences.find(item_id);
        if (it != sExperiences.end())
        {
            return &it->second;
        }
        const LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!fetch || !item)
        {
            return nullptr;
        }
        LLViewerRegion* region = gAgent.getRegion();
        if (!region || region->getCapability("GetMetadata").empty())
        {
            // Not kept: another region may have the capability.
            static SKExperienceInfo sNoCapability;
            sNoCapability.mState = SK_ASSET_FAILED;
            sNoCapability.mNoCapability = true;
            return &sNoCapability;
        }
        SKExperienceInfo& info = sExperiences[item_id];
        const LLUUID id = item_id;
        LLExperienceCache::instance().fetchAssociatedExperience(item->getParentUUID(), item_id, std::string(),
                                                                [id](const LLSD& experience)
                                                                {
                                                                    SKExperienceInfo& entry = sExperiences[id];
                                                                    entry.mState = SK_ASSET_READY;
                                                                    if (experience.has(LLExperienceCache::EXPERIENCE_ID))
                                                                    {
                                                                        entry.mExperienceID = experience[LLExperienceCache::EXPERIENCE_ID].asUUID();
                                                                    }
                                                                    ++sRevision;
                                                                });
        return &info;
    }

    LLLandmark* landmark(const LLUUID& asset_id, bool fetch)
    {
        if (asset_id.isNull())
        {
            return nullptr;
        }
        if (!fetch)
        {
            return gLandmarkList.assetExists(asset_id) ? gLandmarkList.getAsset(asset_id) : nullptr;
        }
        return gLandmarkList.getAsset(asset_id, [](LLLandmark*) { ++sRevision; });
    }
}
