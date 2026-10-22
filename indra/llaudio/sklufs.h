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

// A boost is additionally capped so the clip's sample peak stays under this
// ceiling. Loudness is an average, so transient material -- most SL sounds --
// can read far below target while already peaking near full scale; boosting it
// to hit the target drives the peak past 0 dBFS and the mastering limiter then
// audibly claws it back. EBU R 128 pairs its -23 LUFS target with a -1 dBTP
// ceiling for exactly this reason. Sample peak, not true peak: inter-sample
// overshoot needs oversampling to see, and the 1 dB of margin covers it for
// material at this length.
constexpr F32 SK_LUFS_PEAK_CEILING_DB = -1.f;
constexpr F32 SK_LUFS_PEAK_CEILING    = 0.891251f; // 10^(-1/20)

// Fraction of the correction to actually apply. At 1.0 every clip inside the
// boost/cut limits lands on exactly the target, which flattens the level
// differences a creator chose -- in a virtual world the gap between a whisper
// and a cannon is content, not a mastering error. Half the correction halves
// the spread while leaving the authored ordering and half its magnitude intact.
constexpr F32 SK_LUFS_STRENGTH_DEFAULT = 0.5f;

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

    // Largest absolute sample seen, 0..1. Taken before K-weighting: the shelf
    // adds about 4 dB at high frequencies, so a filtered peak would overstate
    // how much headroom the clip actually consumes.
    F32 peak() const { return mPeak; }

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

    // Gated loudness over sub-block energies grouped `per_block` at a time.
    // Returns SK_LUFS_SILENCE when nothing survives the gates.
    F32 gatedLoudness(size_t per_block) const;

    U32  mSampleRate;
    U32  mChannels;
    bool mValid;

    std::vector<Biquad> mShelf;  // [channel]
    std::vector<Biquad> mHighpass;

    size_t mSubBlockLen;    // frames in a 100 ms sub-block
    size_t mSubBlockFill;
    F64    mSubBlockAcc;    // channel-weighted mean square, summed over the sub-block
    std::vector<F64> mSubBlockEnergy;

    // Whole-clip fallback for material too short to fill even one sub-block.
    F64    mTotalAcc;
    size_t mTotalFrames;

    F32    mPeak;

    // Partial frame carried between feedBytesLE() calls.
    U8     mLeftover[2 * 8];
    size_t mLeftoverBytes;
};

// Linear gain that moves a measured loudness toward the target, clamped to the
// boost/cut limits above. Returns 1.0 for silence or an already-on-target clip.
//
// strength scales the correction in dB before clamping; see
// SK_LUFS_STRENGTH_DEFAULT.
//
// peak is the clip's sample peak (0..1). A boost is capped so peak * gain stays
// under SK_LUFS_PEAK_CEILING; a clip already above the ceiling is simply left
// alone rather than cut, since trimming for peak is a different feature. Pass 0
// when the peak is unknown, which disables the cap.
F32 sk_lufs_gain_for_target(F32 measured_lufs,
                            F32 target_lufs,
                            F32 strength = 1.f,
                            F32 peak = 0.f);

// In-place linear gain on 16-bit PCM, saturating rather than wrapping.
// For the upload path, where the samples themselves are the deliverable.
void sk_lufs_apply_gain16(S16* samples, size_t count, F32 gain);

// Integrated loudness of a RIFF/WAVE file on disk, for pre-upload measurement.
// Returns false when the file could not be read or is not 8/16-bit PCM.
//
// Set downmix_to_mono to measure the channel average rather than the file as
// multichannel. The vorbis encoder downmixes on upload, and a correlated stereo
// pair measures up to 3 LU hotter than its own downmix, so measuring the file
// as-is would solve for a gain the encoded asset never sees.
bool sk_lufs_measure_wav_file(const std::string& path,
                              F32&               out_lufs,
                              F32&               out_peak,
                              bool               downmix_to_mono = false);

#endif // SK_LUFS_H
