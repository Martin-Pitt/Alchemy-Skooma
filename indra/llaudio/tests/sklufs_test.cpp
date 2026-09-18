/**
 * @file sklufs_test.cpp
 * @brief Tests for the BS.1770-4 integrated loudness meter.
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

#include "../sklufs.h"

#include "llmath.h"

#include "../test/lltut.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <vector>

namespace
{
    const U32 TEST_SAMPLE_RATE = 44100;

    // The two K-weighting biquads, evaluated on the unit circle. Duplicated
    // from sklufs.cpp deliberately: the point is to check the meter against
    // the transfer function the coefficients describe, so the expected values
    // come from frequency-domain arithmetic rather than from a second copy of
    // the time-domain filter.
    F64 biquad_gain(F64 b0, F64 b1, F64 b2, F64 a1, F64 a2, F64 freq, F64 fs)
    {
        const std::complex<F64> z = std::exp(std::complex<F64>(0.0, -2.0 * F_PI * freq / fs));
        const std::complex<F64> num = b0 + b1 * z + b2 * z * z;
        const std::complex<F64> den = 1.0 + a1 * z + a2 * z * z;
        return std::abs(num / den);
    }

    F64 k_weighting_gain(F64 freq, F64 fs)
    {
        F64 shelf;
        {
            const F64 f0 = 1681.974450955533;
            const F64 G  = 3.999843853973347;
            const F64 Q  = 0.7071752369554196;
            const F64 K  = tan(F_PI * f0 / fs);
            const F64 Vh = pow(10.0, G / 20.0);
            const F64 Vb = pow(Vh, 0.4996667741545416);
            const F64 a0 = 1.0 + K / Q + K * K;
            shelf = biquad_gain((Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0,
                                (Vh - Vb * K / Q + K * K) / a0, 2.0 * (K * K - 1.0) / a0,
                                (1.0 - K / Q + K * K) / a0, freq, fs);
        }

        F64 highpass;
        {
            const F64 f0 = 38.13547087602444;
            const F64 Q  = 0.5003270373238773;
            const F64 K  = tan(F_PI * f0 / fs);
            const F64 a0 = 1.0 + K / Q + K * K;
            highpass = biquad_gain(1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0, freq, fs);
        }

        return shelf * highpass;
    }

    // Loudness a steady sine of this peak amplitude must measure, per BS.1770:
    // -0.691 + 10*log10(sum over channels of the K-weighted mean square), and a
    // sine's mean square is A^2/2.
    F64 expected_sine_lufs(F64 freq, F64 amplitude, U32 channels, F64 fs = (F64)TEST_SAMPLE_RATE)
    {
        const F64 weighted = k_weighting_gain(freq, fs) * amplitude;
        return -0.691 + 10.0 * log10((F64)channels * weighted * weighted / 2.0);
    }

    // Interleaved 16-bit sine, identical on every channel.
    std::vector<S16> make_sine(F64 freq, F64 amplitude, U32 channels, F64 seconds, U32 fs = TEST_SAMPLE_RATE)
    {
        const size_t frames = (size_t)((F64)fs * seconds);
        std::vector<S16> out(frames * channels);
        for (size_t i = 0; i < frames; ++i)
        {
            const F64 v = amplitude * sin(2.0 * F_PI * freq * (F64)i / (F64)fs);
            const S16 s = (S16)llclamp((S32)ll_round((F32)(v * 32768.0)), -32768, 32767);
            for (U32 c = 0; c < channels; ++c)
            {
                out[i * channels + c] = s;
            }
        }
        return out;
    }

    // Minimal RIFF/WAVE writer, so the on-disk path can be exercised without
    // shipping a binary fixture.
    bool write_wav(const std::string& path, const std::vector<S16>& pcm, U16 channels, U32 sample_rate)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            return false;
        }

        const U32 data_bytes = (U32)(pcm.size() * sizeof(S16));
        const U32 byte_rate  = sample_rate * channels * 2;
        const U16 align      = (U16)(channels * 2);

        auto put32 = [&out](U32 v)
        {
            const char b[4] = { (char)(v & 0xFF), (char)((v >> 8) & 0xFF), (char)((v >> 16) & 0xFF),
                                (char)((v >> 24) & 0xFF) };
            out.write(b, 4);
        };
        auto put16 = [&out](U16 v)
        {
            const char b[2] = { (char)(v & 0xFF), (char)((v >> 8) & 0xFF) };
            out.write(b, 2);
        };

        out.write("RIFF", 4);
        put32(36 + data_bytes);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        put32(16);
        put16(1);            // PCM
        put16(channels);
        put32(sample_rate);
        put32(byte_rate);
        put16(align);
        put16(16);           // bits per sample
        out.write("data", 4);
        put32(data_bytes);

        for (S16 sample : pcm)
        {
            put16((U16)sample);
        }

        return out.good();
    }
}

namespace tut
{
    struct sklufs_test
    {
    };

    typedef test_group<sklufs_test> sklufs_t;
    typedef sklufs_t::object sklufs_object_t;
    tut::sklufs_t tut_sklufs("SKLufsMeter");

    // Steady sines land on the analytic prediction, across frequency and
    // channel count. Tolerance covers filter warm-up and the discarded
    // partial block at the tail.
    template<> template<>
    void sklufs_object_t::test<1>()
    {
        struct Case { F64 freq; F64 amp; U32 channels; };
        const Case cases[] = {
            { 1000.0, 0.10, 1 }, { 1000.0, 0.10, 2 }, { 1000.0, 1.00, 1 },
            {  100.0, 0.50, 1 }, { 4000.0, 0.25, 2 },
        };

        for (const Case& c : cases)
        {
            std::vector<S16> pcm = make_sine(c.freq, c.amp, c.channels, 3.0);
            SKLufsMeter meter(TEST_SAMPLE_RATE, c.channels);
            meter.feed(pcm.data(), pcm.size() / c.channels);

            const F64 expected = expected_sine_lufs(c.freq, c.amp, c.channels);
            std::string what = llformat("%.0f Hz sine, amp %.2f, %u ch", c.freq, c.amp, c.channels);
            ensure_approximately_equals(what.c_str(), (F32)meter.close(), (F32)expected, 8);
        }
    }

    // A 1 kHz sine at 0.1 peak is the reference point the rest hangs off:
    // it should come out at -23 LUFS, which is also the default target.
    template<> template<>
    void sklufs_object_t::test<2>()
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.1, 1, 3.0);
        SKLufsMeter meter(TEST_SAMPLE_RATE, 1);
        meter.feed(pcm.data(), pcm.size());
        ensure_approximately_equals("1 kHz at 0.1 peak measures -23 LUFS", meter.close(), -23.f, 6);
    }

    // Most SL sounds are shorter than one 400 ms gating block. They must still
    // measure, not report silence.
    template<> template<>
    void sklufs_object_t::test<3>()
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.1, 1, 0.2);
        SKLufsMeter meter(TEST_SAMPLE_RATE, 1);
        meter.feed(pcm.data(), pcm.size());

        const F32 lufs = meter.close();
        ensure("short clip is not reported as silence", lufs > SK_LUFS_SILENCE);
        ensure_approximately_equals("short clip measures correctly", lufs, -23.f, 5);
    }

    // The decode loop feeds whatever chunking ov_read() hands back, so chunked
    // feeding must agree with one-shot feeding even when a chunk ends in the
    // middle of a stereo frame.
    template<> template<>
    void sklufs_object_t::test<4>()
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.1, 2, 1.0);

        SKLufsMeter whole(TEST_SAMPLE_RATE, 2);
        whole.feed(pcm.data(), pcm.size() / 2);
        const F32 reference = whole.close();

        const U8* bytes = (const U8*)pcm.data();
        const size_t byte_count = pcm.size() * sizeof(S16);
        for (size_t chunk : { (size_t)1, (size_t)3, (size_t)4095, (size_t)4096 })
        {
            SKLufsMeter meter(TEST_SAMPLE_RATE, 2);
            for (size_t off = 0; off < byte_count; off += chunk)
            {
                meter.feedBytesLE(bytes + off, llmin(chunk, byte_count - off));
            }
            std::string what = llformat("feedBytesLE in %u-byte chunks", (U32)chunk);
            ensure_approximately_equals(what.c_str(), meter.close(), reference, 16);
        }
    }

    // Digital silence gates out entirely, and an empty feed is not a crash.
    template<> template<>
    void sklufs_object_t::test<5>()
    {
        std::vector<S16> quiet(TEST_SAMPLE_RATE, 0);
        SKLufsMeter meter(TEST_SAMPLE_RATE, 1);
        meter.feed(quiet.data(), quiet.size());
        ensure_equals("silence measures as silence", meter.close(), SK_LUFS_SILENCE);

        SKLufsMeter empty(TEST_SAMPLE_RATE, 1);
        empty.feed(nullptr, 0);
        empty.feedBytesLE(nullptr, 0);
        ensure_equals("empty feed measures as silence", empty.close(), SK_LUFS_SILENCE);

        SKLufsMeter bogus(0, 0);
        ensure("degenerate geometry is rejected", !bogus.isValid());
    }

    // Solving for a target, and the clamps that keep a pathological
    // measurement from dragging up a noise floor or muting a sound.
    template<> template<>
    void sklufs_object_t::test<6>()
    {
        ensure_approximately_equals("7 LU boost is a linear 10^(7/20)",
                                    sk_lufs_gain_for_target(-30.f, -23.f), powf(10.f, 7.f / 20.f), 16);
        ensure_equals("already on target leaves gain at unity",
                      sk_lufs_gain_for_target(-23.2f, -23.f), 1.f);
        ensure_equals("silence leaves gain at unity",
                      sk_lufs_gain_for_target(SK_LUFS_SILENCE, -23.f), 1.f);
        ensure_approximately_equals("boost is clamped", sk_lufs_gain_for_target(-80.f, -23.f),
                                    powf(10.f, SK_LUFS_MAX_BOOST_DB / 20.f), 16);
        ensure_approximately_equals("cut is clamped", sk_lufs_gain_for_target(0.f, -60.f),
                                    powf(10.f, SK_LUFS_MAX_CUT_DB / 20.f), 16);
    }

    // Applying the solved gain re-measures on target: the round trip the
    // upload path depends on.
    template<> template<>
    void sklufs_object_t::test<7>()
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.1, 1, 3.0);

        SKLufsMeter before(TEST_SAMPLE_RATE, 1);
        before.feed(pcm.data(), pcm.size());
        const F32 measured = before.close();

        sk_lufs_apply_gain16(pcm.data(), pcm.size(), sk_lufs_gain_for_target(measured, -18.f));

        SKLufsMeter after(TEST_SAMPLE_RATE, 1);
        after.feed(pcm.data(), pcm.size());
        ensure_approximately_equals("gain round trip lands on target", after.close(), -18.f, 6);
    }

    // The on-disk path, used both for cache hits and for pre-upload
    // measurement, including the mono downmix the vorbis encoder performs.
    template<> template<>
    void sklufs_object_t::test<8>()
    {
        const std::string path = "sklufs_test_tone.wav";

        std::vector<S16> mono = make_sine(1000.0, 0.1, 1, 1.0);
        ensure("wrote mono test wav", write_wav(path, mono, 1, TEST_SAMPLE_RATE));

        F32 lufs = SK_LUFS_SILENCE;
        ensure("measured mono wav from disk", sk_lufs_measure_wav_file(path, lufs));
        ensure_approximately_equals("mono wav measures -23 LUFS", lufs, -23.f, 6);

        // Identical channels: as stereo the powers sum (+3 LU); downmixed they
        // average back to the mono level.
        std::vector<S16> stereo = make_sine(1000.0, 0.1, 2, 1.0);
        ensure("wrote stereo test wav", write_wav(path, stereo, 2, TEST_SAMPLE_RATE));

        F32 as_stereo = SK_LUFS_SILENCE;
        ensure("measured stereo wav", sk_lufs_measure_wav_file(path, as_stereo, false));
        ensure_approximately_equals("correlated stereo is 3 LU hotter", as_stereo, -20.f, 6);

        F32 downmixed = SK_LUFS_SILENCE;
        ensure("measured stereo wav downmixed", sk_lufs_measure_wav_file(path, downmixed, true));
        ensure_approximately_equals("downmix matches the mono level", downmixed, -23.f, 6);

        std::remove(path.c_str());

        F32 unused = SK_LUFS_SILENCE;
        ensure("a missing file is a failure, not silence",
               !sk_lufs_measure_wav_file("sklufs_test_does_not_exist.wav", unused));
    }
}
