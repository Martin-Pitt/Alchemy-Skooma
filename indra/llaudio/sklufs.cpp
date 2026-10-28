/**
 * @file sklufs.cpp
 * @brief ITU-R BS.1770-4 integrated loudness measurement for 16-bit PCM.
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

    // F_PI is single precision, which moves the K-weighting off BS.1770-4's table.
    constexpr F64 SK_LUFS_PI = 3.14159265358979323846;

    constexpr F64 SK_LUFS_ABS_GATE     = -70.0;
    constexpr F64 SK_LUFS_REL_GATE_LU  = -10.0;

    // Clips up to this long read as their loudest 400 ms; from the long bound
    // up they read as integrated loudness, blended in between.
    constexpr F64 SK_LUFS_SHORT_SECONDS = 3.0;
    constexpr F64 SK_LUFS_LONG_SECONDS  = 10.0;

    // Integrated loudness is held no lower than this far under the loudest momentary block.
    constexpr F64 SK_LUFS_MOMENTARY_FLOOR_LU = 8.0;

    constexpr size_t SUB_BLOCKS_PER_BLOCK = 4;

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

// static
void SKLufsMeter::kWeighting(F64 fs, F64 shelf[5], F64 highpass[5])
{
    // Stage 1: high shelf.
    {
        const F64 f0 = 1681.974450955533;
        const F64 G  = 3.999843853973347;
        const F64 Q  = 0.7071752369554196;

        const F64 K  = tan(SK_LUFS_PI * f0 / fs);
        const F64 Vh = pow(10.0, G / 20.0);
        const F64 Vb = pow(Vh, 0.4996667741545416);
        const F64 a0 = 1.0 + K / Q + K * K;

        shelf[0] = (Vh + Vb * K / Q + K * K) / a0;
        shelf[1] = 2.0 * (K * K - Vh) / a0;
        shelf[2] = (Vh - Vb * K / Q + K * K) / a0;
        shelf[3] = 2.0 * (K * K - 1.0) / a0;
        shelf[4] = (1.0 - K / Q + K * K) / a0;
    }

    // Stage 2: RLB high pass.
    {
        const F64 f0 = 38.13547087602444;
        const F64 Q  = 0.5003270373238773;

        const F64 K  = tan(SK_LUFS_PI * f0 / fs);
        const F64 a0 = 1.0 + K / Q + K * K;

        highpass[0] = 1.0;
        highpass[1] = -2.0;
        highpass[2] = 1.0;
        highpass[3] = 2.0 * (K * K - 1.0) / a0;
        highpass[4] = (1.0 - K / Q + K * K) / a0;
    }
}

SKLufsMeter::SKLufsMeter(U32 sample_rate, U32 num_channels)
:   mSampleRate(sample_rate),
    mChannels(num_channels),
    mValid(sample_rate > 0 && num_channels > 0 && num_channels <= SK_LUFS_MAX_CHANNELS),
    mSubBlockLen(0),
    mSubBlockFill(0),
    mSubBlockAcc(0.0),
    mTotalFrames(0),
    mPeak(0.f),
    mLeftoverBytes(0)
{
    if (!mValid)
    {
        return;
    }

    Biquad shelf;
    Biquad highpass;
    F64 sc[5];
    F64 hc[5];
    kWeighting((F64)mSampleRate, sc, hc);
    shelf.b0 = sc[0]; shelf.b1 = sc[1]; shelf.b2 = sc[2]; shelf.a1 = sc[3]; shelf.a2 = sc[4];
    highpass.b0 = hc[0]; highpass.b1 = hc[1]; highpass.b2 = hc[2]; highpass.a1 = hc[3]; highpass.a2 = hc[4];

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
        mPeak = llmax(mPeak, (F32)fabs(x)); // before weighting; see peak()
        const F64 y = mHighpass[c].process(mShelf[c].process(x));
        frame_power += channel_weight(c) * y * y;
    }

    mSubBlockAcc += frame_power;
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

F32 SKLufsMeter::gatedLoudness(const std::vector<F64>& powers)
{
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

F32 SKLufsMeter::close() const
{
    if (!mValid || !mTotalFrames)
    {
        return SK_LUFS_SILENCE;
    }

    // The unfinished tail counts as a zero-padded sub-block, and a clip shorter
    // than one gating block is padded out to one, so padding never moves M.
    std::vector<F64> energy = mSubBlockEnergy;
    if (mSubBlockFill)
    {
        energy.push_back(mSubBlockAcc);
    }
    while (energy.size() < SUB_BLOCKS_PER_BLOCK)
    {
        energy.push_back(0.0);
    }

    // 400 ms gating blocks at a 100 ms hop.
    const F64 block_div = (F64)(SUB_BLOCKS_PER_BLOCK * mSubBlockLen);
    std::vector<F64> powers;
    powers.reserve(energy.size() - SUB_BLOCKS_PER_BLOCK + 1);
    F64 loudest = 0.0;
    for (size_t j = 0; j + SUB_BLOCKS_PER_BLOCK <= energy.size(); ++j)
    {
        F64 sum = 0.0;
        for (size_t k = 0; k < SUB_BLOCKS_PER_BLOCK; ++k)
        {
            sum += energy[j + k];
        }
        powers.push_back(sum / block_div);
        loudest = llmax(loudest, powers.back());
    }

    // Maximum momentary loudness, under the absolute gate like every reading.
    if (loudest <= 0.0 || block_loudness(loudest) <= SK_LUFS_ABS_GATE)
    {
        return SK_LUFS_SILENCE;
    }
    const F64 momentary = block_loudness(loudest);

    const F64 seconds = (F64)mTotalFrames / (F64)mSampleRate;
    if (seconds <= SK_LUFS_SHORT_SECONDS)
    {
        return (F32)momentary;
    }

    // Integrated loudness, floored so a lone hit in a long quiet clip is not diluted to nothing.
    const F32 integrated = gatedLoudness(powers);
    const F64 floored = integrated > SK_LUFS_SILENCE
        ? llmax((F64)integrated, momentary - SK_LUFS_MOMENTARY_FLOOR_LU)
        : momentary - SK_LUFS_MOMENTARY_FLOOR_LU;
    if (seconds >= SK_LUFS_LONG_SECONDS)
    {
        return (F32)floored;
    }

    const F64 w = (seconds - SK_LUFS_SHORT_SECONDS) / (SK_LUFS_LONG_SECONDS - SK_LUFS_SHORT_SECONDS);
    return (F32)((1.0 - w) * momentary + w * floored);
}

F32 sk_lufs_gain_for_target(F32 measured_lufs, F32 target_lufs, F32 strength, F32 peak)
{
    if (measured_lufs <= SK_LUFS_SILENCE)
    {
        return 1.f;
    }

    // Scale before the tolerance test, so a clip already near the target is not
    // nudged by a fraction of a dB for nothing.
    F32 delta_db = llclamp(strength, 0.f, 1.f) * (target_lufs - measured_lufs);
    if (fabsf(delta_db) <= SK_LUFS_TOLERANCE)
    {
        return 1.f;
    }

    delta_db = llclamp(delta_db, SK_LUFS_MAX_CUT_DB, SK_LUFS_MAX_BOOST_DB);

    // Loudness is a power measure, so a dB delta maps to an amplitude ratio
    // through /20, not /10.
    F32 gain = powf(10.f, delta_db / 20.f);

    // Never boost a clip past the peak ceiling. A clip already over it keeps
    // unity rather than being cut -- that would be a limiter, not this.
    if (gain > 1.f && peak > 0.f)
    {
        gain = llclamp(gain, 1.f, llmax(1.f, SK_LUFS_PEAK_CEILING / peak));
    }

    return gain;
}

bool sk_lufs_measure_wav_file(const std::string& path, F32& out_lufs, F32& out_peak)
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

    if (info.bits_per_sample != 8 && info.bits_per_sample != 16)
    {
        return false;
    }

    // Widen everything to interleaved signed 16-bit first, so the format
    // handling lives in one place. 8-bit WAV is unsigned and centred on 128.
    const size_t bytes_per_sample = info.bits_per_sample / 8;
    const size_t total_samples    = info.pcm.size() / bytes_per_sample;
    const size_t frames           = total_samples / info.channels;
    if (!frames)
    {
        return false;
    }

    std::vector<S16> pcm(total_samples);
    if (info.bits_per_sample == 16)
    {
        for (size_t i = 0; i < total_samples; ++i)
        {
            pcm[i] = (S16)((U16)info.pcm[i * 2] | ((U16)info.pcm[i * 2 + 1] << 8));
        }
    }
    else
    {
        for (size_t i = 0; i < total_samples; ++i)
        {
            pcm[i] = (S16)(((S32)info.pcm[i] - 128) << 8);
        }
    }

    SKLufsMeter meter(info.sample_rate, info.channels);
    if (!meter.isValid())
    {
        return false;
    }

    meter.feed(pcm.data(), frames);
    out_lufs = meter.close();
    out_peak = meter.peak();
    return true;
}
