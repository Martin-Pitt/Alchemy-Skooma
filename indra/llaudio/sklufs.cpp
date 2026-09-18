/**
 * @file sklufs.cpp
 * @brief ITU-R BS.1770-4 integrated loudness measurement for 16-bit PCM.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project
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

#include "sklufs.h"

#include "llmath.h"
#include "llwavfile.h"

#include <cmath>

namespace
{
    // Maximum channels the meter will weight; SL clips are mono or stereo, but
    // the leftover buffer and frame scratch are sized for this.
    constexpr U32 SK_LUFS_MAX_CHANNELS = 8;

    constexpr F64 SK_LUFS_ABS_GATE     = -70.0;
    constexpr F64 SK_LUFS_REL_GATE_LU  = -10.0;

    // BS.1770-4 block loudness from mean square.
    inline F64 block_loudness(F64 power)
    {
        return -0.691 + 10.0 * log10(power);
    }

    // BS.1770-4 channel weights: L/R/C unity, surrounds +1.5 dB. Anything
    // beyond the defined layout is left at unity.
    inline F64 channel_weight(U32 channel)
    {
        return (channel == 3 || channel == 4) ? 1.41 : 1.0;
    }
}

SKLufsMeter::SKLufsMeter(U32 sample_rate, U32 num_channels)
:   mSampleRate(sample_rate),
    mChannels(num_channels),
    mValid(sample_rate > 0 && num_channels > 0 && num_channels <= SK_LUFS_MAX_CHANNELS),
    mSubBlockLen(0),
    mSubBlockFill(0),
    mSubBlockAcc(0.0),
    mTotalAcc(0.0),
    mTotalFrames(0),
    mLeftoverBytes(0)
{
    if (!mValid)
    {
        return;
    }

    const F64 fs = (F64)mSampleRate;

    // Stage 1: high shelf.
    Biquad shelf;
    {
        const F64 f0 = 1681.974450955533;
        const F64 G  = 3.999843853973347;
        const F64 Q  = 0.7071752369554196;

        const F64 K  = tan(F_PI * f0 / fs);
        const F64 Vh = pow(10.0, G / 20.0);
        const F64 Vb = pow(Vh, 0.4996667741545416);
        const F64 a0 = 1.0 + K / Q + K * K;

        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf.b1 = 2.0 * (K * K - Vh) / a0;
        shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }

    // Stage 2: RLB high pass.
    Biquad highpass;
    {
        const F64 f0 = 38.13547087602444;
        const F64 Q  = 0.5003270373238773;

        const F64 K  = tan(F_PI * f0 / fs);
        const F64 a0 = 1.0 + K / Q + K * K;

        highpass.b0 = 1.0;
        highpass.b1 = -2.0;
        highpass.b2 = 1.0;
        highpass.a1 = 2.0 * (K * K - 1.0) / a0;
        highpass.a2 = (1.0 - K / Q + K * K) / a0;
    }

    mShelf.assign(mChannels, shelf);
    mHighpass.assign(mChannels, highpass);

    // A gating block is 400 ms; overlapping them 75% means accumulating in
    // 100 ms sub-blocks and summing four consecutive ones per block.
    mSubBlockLen = mSampleRate / 10;
    if (!mSubBlockLen)
    {
        mValid = false;
    }
}

void SKLufsMeter::pushFrame(const S16* frame)
{
    F64 frame_power = 0.0;
    for (U32 c = 0; c < mChannels; ++c)
    {
        const F64 x = (F64)frame[c] / 32768.0;
        const F64 y = mHighpass[c].process(mShelf[c].process(x));
        frame_power += channel_weight(c) * y * y;
    }

    mSubBlockAcc += frame_power;
    mTotalAcc += frame_power;
    mTotalFrames++;

    if (++mSubBlockFill == mSubBlockLen)
    {
        mSubBlockEnergy.push_back(mSubBlockAcc);
        mSubBlockAcc = 0.0;
        mSubBlockFill = 0;
    }
}

void SKLufsMeter::feed(const S16* frames, size_t frame_count)
{
    if (!mValid || !frames)
    {
        return;
    }

    for (size_t i = 0; i < frame_count; ++i)
    {
        pushFrame(frames + i * mChannels);
    }
}

void SKLufsMeter::feedBytesLE(const U8* bytes, size_t byte_count)
{
    if (!mValid || !bytes || !byte_count)
    {
        return;
    }

    const size_t frame_bytes = (size_t)mChannels * 2;
    S16 frame[SK_LUFS_MAX_CHANNELS];
    size_t i = 0;

    // Complete a frame carried over from the previous chunk first.
    if (mLeftoverBytes)
    {
        while (mLeftoverBytes < frame_bytes && i < byte_count)
        {
            mLeftover[mLeftoverBytes++] = bytes[i++];
        }
        if (mLeftoverBytes < frame_bytes)
        {
            return;
        }
        for (U32 c = 0; c < mChannels; ++c)
        {
            frame[c] = (S16)((U16)mLeftover[c * 2] | ((U16)mLeftover[c * 2 + 1] << 8));
        }
        pushFrame(frame);
        mLeftoverBytes = 0;
    }

    while (i + frame_bytes <= byte_count)
    {
        for (U32 c = 0; c < mChannels; ++c)
        {
            frame[c] = (S16)((U16)bytes[i + c * 2] | ((U16)bytes[i + c * 2 + 1] << 8));
        }
        pushFrame(frame);
        i += frame_bytes;
    }

    while (i < byte_count)
    {
        mLeftover[mLeftoverBytes++] = bytes[i++];
    }
}

F32 SKLufsMeter::close() const
{
    if (!mValid || !mTotalFrames)
    {
        return SK_LUFS_SILENCE;
    }

    std::vector<F64> powers;
    if (mSubBlockEnergy.size() >= 4)
    {
        const F64 block_div = (F64)(4 * mSubBlockLen);
        powers.reserve(mSubBlockEnergy.size() - 3);
        for (size_t j = 0; j + 4 <= mSubBlockEnergy.size(); ++j)
        {
            powers.push_back((mSubBlockEnergy[j] + mSubBlockEnergy[j + 1] + mSubBlockEnergy[j + 2] +
                              mSubBlockEnergy[j + 3]) /
                             block_div);
        }
    }
    else
    {
        // Shorter than one gating block, which most SL sounds are. Measure the
        // whole clip as a single block rather than reporting silence.
        powers.push_back(mTotalAcc / (F64)mTotalFrames);
    }

    // First pass: absolute gate only, to establish the relative threshold.
    F64    sum = 0.0;
    size_t count = 0;
    for (F64 p : powers)
    {
        if (p > 0.0 && block_loudness(p) > SK_LUFS_ABS_GATE)
        {
            sum += p;
            count++;
        }
    }
    if (!count)
    {
        return SK_LUFS_SILENCE;
    }

    // Second pass: both gates.
    const F64 rel_gate = block_loudness(sum / (F64)count) + SK_LUFS_REL_GATE_LU;
    F64    gated_sum = 0.0;
    size_t gated_count = 0;
    for (F64 p : powers)
    {
        if (p <= 0.0)
        {
            continue;
        }
        const F64 l = block_loudness(p);
        if (l > SK_LUFS_ABS_GATE && l > rel_gate)
        {
            gated_sum += p;
            gated_count++;
        }
    }
    if (!gated_count)
    {
        return SK_LUFS_SILENCE;
    }

    return (F32)block_loudness(gated_sum / (F64)gated_count);
}

F32 sk_lufs_gain_for_target(F32 measured_lufs, F32 target_lufs)
{
    if (measured_lufs <= SK_LUFS_SILENCE)
    {
        return 1.f;
    }

    F32 delta_db = target_lufs - measured_lufs;
    if (fabsf(delta_db) <= SK_LUFS_TOLERANCE)
    {
        return 1.f;
    }

    delta_db = llclamp(delta_db, SK_LUFS_MAX_CUT_DB, SK_LUFS_MAX_BOOST_DB);

    // Loudness is a power measure, so a dB delta maps to an amplitude ratio
    // through /20, not /10.
    return powf(10.f, delta_db / 20.f);
}

void sk_lufs_apply_gain16(S16* samples, size_t count, F32 gain)
{
    if (!samples || gain == 1.f)
    {
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        const S32 v = ll_round((F32)samples[i] * gain);
        samples[i] = (S16)llclamp(v, -32768, 32767);
    }
}

bool sk_lufs_measure_wav_file(const std::string& path, F32& out_lufs)
{
    LLAudio::WavInfo info;
    if (!LLAudio::parseWav(path, info) || info.format_tag != 1 || info.pcm.empty())
    {
        return false;
    }

    if (!info.channels || !info.sample_rate || info.channels > SK_LUFS_MAX_CHANNELS)
    {
        return false;
    }

    SKLufsMeter meter(info.sample_rate, info.channels);
    if (!meter.isValid())
    {
        return false;
    }

    if (info.bits_per_sample == 16)
    {
        meter.feedBytesLE(info.pcm.data(), info.pcm.size());
    }
    else if (info.bits_per_sample == 8)
    {
        // 8-bit WAV is unsigned and centred on 128; widen to the meter's
        // signed 16-bit input rather than teaching the meter a second format.
        std::vector<S16> widened(info.pcm.size());
        for (size_t i = 0; i < info.pcm.size(); ++i)
        {
            widened[i] = (S16)(((S32)info.pcm[i] - 128) << 8);
        }
        meter.feed(widened.data(), widened.size() / info.channels);
    }
    else
    {
        return false;
    }

    out_lufs = meter.close();
    return true;
}
