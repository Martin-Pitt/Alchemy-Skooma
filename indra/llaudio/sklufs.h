/**
 * @file sklufs.h
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

#ifndef SK_LUFS_H
#define SK_LUFS_H

#include "stdtypes.h"

#include <cstddef>
#include <string>
#include <vector>

// Returned when nothing in the clip rose above the absolute gate.
constexpr F32 SK_LUFS_SILENCE = -100.f;

// EBU R 128. ReplayGain/AES uses -18 and streaming services -14..-16, so this
// is deliberately a setting rather than a hard-coded constant at the call sites.
constexpr F32 SK_LUFS_TARGET_DEFAULT = -23.f;

// Leave a clip alone when it already measures this close to the target.
constexpr F32 SK_LUFS_TOLERANCE = 0.5f;

// Correction limits. Without a ceiling, a clip mastered 40 dB low would have its
// noise floor dragged up with it; without a floor, one pathological measurement
// could mute a sound outright.
constexpr F32 SK_LUFS_MAX_BOOST_DB = 12.f;
constexpr F32 SK_LUFS_MAX_CUT_DB   = -24.f;

/**
 * Incremental integrated-loudness meter, BS.1770-4.
 *
 * K-weighting (high shelf + RLB high pass) per channel, mean square over 400 ms
 * gating blocks at 75% overlap, absolute gate at -70 LUFS and a relative gate
 * 10 LU below the ungated mean.
 *
 * Incremental so it can be driven from a decode loop that is already streaming
 * the samples past, rather than costing a second pass over the buffer. Feed
 * interleaved frames in any chunking; a chunk that ends mid-frame is carried
 * over to the next feed().
 */
class SKLufsMeter
{
public:
    SKLufsMeter(U32 sample_rate, U32 num_channels);

    // Interleaved 16-bit frames. Safe to call with any count, including 0.
    void feed(const S16* frames, size_t frame_count);

    // Same, from a raw little-endian byte run. A trailing partial frame is
    // buffered until the next call.
    void feedBytesLE(const U8* bytes, size_t byte_count);

    // Integrated loudness over everything fed so far, or SK_LUFS_SILENCE.
    // Non-destructive: feeding may continue afterwards.
    F32 close() const;

    bool isValid() const { return mValid; }

private:
    // Direct-form-I biquad, one per channel per K-weighting stage.
    struct Biquad
    {
        F64 b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        F64 x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

        F64 process(F64 x)
        {
            const F64 y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };

    void pushFrame(const S16* frame);

    U32  mSampleRate;
    U32  mChannels;
    bool mValid;

    std::vector<Biquad> mShelf;  // [channel]
    std::vector<Biquad> mHighpass;

    size_t mSubBlockLen;    // frames in a 100 ms sub-block
    size_t mSubBlockFill;
    F64    mSubBlockAcc;    // channel-weighted mean square, summed over the sub-block
    std::vector<F64> mSubBlockEnergy;

    // Whole-clip fallback for material shorter than one 400 ms gating block.
    F64    mTotalAcc;
    size_t mTotalFrames;

    // Partial frame carried between feedBytesLE() calls.
    U8     mLeftover[2 * 8];
    size_t mLeftoverBytes;
};

// Linear gain that moves a measured loudness onto the target, clamped to the
// boost/cut limits above. Returns 1.0 for silence or an already-on-target clip.
F32 sk_lufs_gain_for_target(F32 measured_lufs, F32 target_lufs);

// In-place linear gain on 16-bit PCM, saturating rather than wrapping.
// For the upload path, where the samples themselves are the deliverable.
void sk_lufs_apply_gain16(S16* samples, size_t count, F32 gain);

// Integrated loudness of a RIFF/WAVE file on disk, for pre-upload measurement.
// Returns false when the file could not be read or is not 8/16-bit PCM.
bool sk_lufs_measure_wav_file(const std::string& path, F32& out_lufs);

#endif // SK_LUFS_H
