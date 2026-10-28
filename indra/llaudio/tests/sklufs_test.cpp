/**
 * @file sklufs_test.cpp
 * @brief Tests for the BS.1770-4 integrated loudness meter.
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

    // The meter's K-weighting evaluated on the unit circle, so steady sines can be checked against
    // frequency-domain arithmetic; the coefficients themselves are checked against BS.1770-4's table.
    F64 biquad_gain(const F64 c[5], F64 freq, F64 fs)
    {
        const std::complex<F64> z = std::exp(std::complex<F64>(0.0, -2.0 * F_PI * freq / fs));
        const std::complex<F64> num = c[0] + c[1] * z + c[2] * z * z;
        const std::complex<F64> den = 1.0 + c[3] * z + c[4] * z * z;
        return std::abs(num / den);
    }

    F64 k_weighting_gain(F64 freq, F64 fs)
    {
        F64 shelf[5];
        F64 highpass[5];
        SKLufsMeter::kWeighting(fs, shelf, highpass);
        return biquad_gain(shelf, freq, fs) * biquad_gain(highpass, freq, fs);
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

    F32 measure_mono(const std::vector<F64>& x, U32 fs = TEST_SAMPLE_RATE)
    {
        std::vector<S16> pcm(x.size());
        for (size_t i = 0; i < x.size(); ++i)
        {
            pcm[i] = (S16)llclamp((S32)ll_round((F32)(x[i] * 32768.0)), -32768, 32767);
        }
        SKLufsMeter meter(fs, 1);
        meter.feed(pcm.data(), pcm.size());
        return meter.close();
    }

    // 60 ms 2 kHz click with a 12 ms decay.
    std::vector<F64> make_click()
    {
        const size_t n = TEST_SAMPLE_RATE * 6 / 100;
        std::vector<F64> x(n);
        for (size_t i = 0; i < n; ++i)
        {
            const F64 t = (F64)i / TEST_SAMPLE_RATE;
            x[i] = 0.9 * sin(2.0 * F_PI * 2000.0 * t) * exp(-t / 0.012);
        }
        return x;
    }

    // 300 ms decaying noise hit, from a fixed-seed generator so the test is deterministic.
    std::vector<F64> make_hit()
    {
        const size_t n = TEST_SAMPLE_RATE * 3 / 10;
        std::vector<F64> x(n);
        U32 state = 1;
        for (size_t i = 0; i < n; ++i)
        {
            F64 sum = 0.0;
            for (int k = 0; k < 12; ++k)
            {
                state = state * 1664525u + 1013904223u;
                sum += (F64)state / 4294967296.0;
            }
            x[i] = (sum - 6.0) * 0.3 * exp(-(F64)i / (TEST_SAMPLE_RATE * 0.08));
        }
        return x;
    }

    // Spread of the reading as the sound is padded with trailing silence from 0.1 s to 4 s.
    F32 padding_spread(const std::vector<F64>& sound)
    {
        F32 lo = 1000.f;
        F32 hi = -1000.f;
        for (int step = 0; step <= 195; ++step)
        {
            const size_t total = (size_t)(TEST_SAMPLE_RATE * (0.1 + 0.02 * step));
            if (total < sound.size())
            {
                continue;
            }
            std::vector<F64> x(sound);
            x.resize(total, 0.0);
            const F32 l = measure_mono(x);
            lo = llmin(lo, l);
            hi = llmax(hi, l);
        }
        return hi - lo;
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
    // channel count. Tolerance covers filter warm-up.
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

    // A clip shorter than one 400 ms block is padded to one: 200 ms of a
    // -23 LUFS tone reads 3 LU under it, not silence.
    template<> template<>
    void sklufs_object_t::test<3>()
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.1, 1, 0.2);
        SKLufsMeter meter(TEST_SAMPLE_RATE, 1);
        meter.feed(pcm.data(), pcm.size());

        const F32 lufs = meter.close();
        ensure("short clip is not reported as silence", lufs > SK_LUFS_SILENCE);
        ensure_approximately_equals("short clip reads its energy over 400 ms", lufs, -23.f - 3.01f, 4);
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

    // Applying the solved gain re-measures on target.
    template<> template<>
    void sklufs_object_t::test<7>()
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.1, 1, 3.0);

        SKLufsMeter before(TEST_SAMPLE_RATE, 1);
        before.feed(pcm.data(), pcm.size());
        const F32 measured = before.close();

        const F32 gain = sk_lufs_gain_for_target(measured, -18.f);
        for (S16& v : pcm)
        {
            v = (S16)llclamp((S32)ll_round((F32)v * gain), -32768, 32767);
        }

        SKLufsMeter after(TEST_SAMPLE_RATE, 1);
        after.feed(pcm.data(), pcm.size());
        ensure_approximately_equals("gain round trip lands on target", after.close(), -18.f, 6);
    }

    // The on-disk path the cache hits use, measured as the header describes the file.
    template<> template<>
    void sklufs_object_t::test<8>()
    {
        const std::string path = "sklufs_test_tone.wav";

        std::vector<S16> mono = make_sine(1000.0, 0.1, 1, 1.0);
        ensure("wrote mono test wav", write_wav(path, mono, 1, TEST_SAMPLE_RATE));

        F32 lufs = SK_LUFS_SILENCE;
        F32 peak = 0.f;
        ensure("measured mono wav from disk", sk_lufs_measure_wav_file(path, lufs, peak));
        ensure_approximately_equals("mono wav measures -23 LUFS", lufs, -23.f, 6);

        // Identical channels: as stereo the powers sum (+3 LU).
        std::vector<S16> stereo = make_sine(1000.0, 0.1, 2, 1.0);
        ensure("wrote stereo test wav", write_wav(path, stereo, 2, TEST_SAMPLE_RATE));

        F32 as_stereo = SK_LUFS_SILENCE;
        ensure("measured stereo wav", sk_lufs_measure_wav_file(path, as_stereo, peak));
        ensure_approximately_equals("correlated stereo is 3 LU hotter", as_stereo, -20.f, 6);

        std::remove(path.c_str());

        F32 unused = SK_LUFS_SILENCE;
        ensure("a missing file is a failure, not silence",
               !sk_lufs_measure_wav_file("sklufs_test_does_not_exist.wav", unused, peak));
    }

    // Strength scales the correction in dB, so half strength is the square
    // root of the full linear gain, and the authored ordering survives.
    template<> template<>
    void sklufs_object_t::test<9>()
    {
        const F32 full = sk_lufs_gain_for_target(-33.f, -23.f, 1.f);
        const F32 half = sk_lufs_gain_for_target(-33.f, -23.f, 0.5f);
        ensure_approximately_equals("full strength is +10 dB", full, powf(10.f, 10.f / 20.f), 16);
        ensure_approximately_equals("half strength is +5 dB", half, powf(10.f, 5.f / 20.f), 16);

        ensure_equals("zero strength is a no-op", sk_lufs_gain_for_target(-40.f, -23.f, 0.f), 1.f);

        // Two clips 10 LU apart stay 5 LU apart at half strength.
        const F32 quiet = sk_lufs_gain_for_target(-33.f, -23.f, 0.5f);
        const F32 loud  = sk_lufs_gain_for_target(-23.f, -23.f, 0.5f);
        const F32 gap   = 20.f * log10f(quiet / loud) ;
        ensure_approximately_equals("a 10 LU authored gap halves to 5 LU", 10.f - gap, 5.f, 5);
    }

    // The peak cap: a boost may not drive the sample peak past the ceiling,
    // and a clip already over it is left alone rather than cut.
    template<> template<>
    void sklufs_object_t::test<10>()
    {
        // Peaking at -1 dBFS exactly: no headroom left, no boost.
        ensure_equals("no headroom means no boost",
                      sk_lufs_gain_for_target(-40.f, -23.f, 1.f, SK_LUFS_PEAK_CEILING), 1.f);

        // Peaking at full scale: still no boost, and never a cut.
        ensure_equals("full-scale peak is left alone",
                      sk_lufs_gain_for_target(-40.f, -23.f, 1.f, 1.f), 1.f);

        // Half-scale peak (-6 dBFS) leaves 5 dB of headroom, so a 17 dB
        // request is capped to that.
        const F32 capped = sk_lufs_gain_for_target(-40.f, -23.f, 1.f, 0.5f);
        ensure_approximately_equals("boost capped to available headroom",
                                    0.5f * capped, SK_LUFS_PEAK_CEILING, 16);

        // Plenty of headroom: the cap does not bind.
        ensure_approximately_equals("headroom to spare leaves the boost alone",
                                    sk_lufs_gain_for_target(-30.f, -23.f, 1.f, 0.01f),
                                    powf(10.f, 7.f / 20.f), 16);

        // A cut is never capped -- the ceiling only limits boosts.
        ensure_approximately_equals("cuts ignore the ceiling",
                                    sk_lufs_gain_for_target(-10.f, -23.f, 1.f, 1.f),
                                    powf(10.f, -13.f / 20.f), 16);

        // Unknown peak disables the cap.
        ensure_approximately_equals("peak 0 disables the cap",
                                    sk_lufs_gain_for_target(-30.f, -23.f, 1.f, 0.f),
                                    powf(10.f, 7.f / 20.f), 16);
    }

    // Peak is taken before K-weighting, and reported over everything fed.
    template<> template<>
    void sklufs_object_t::test<11>()
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.5, 1, 1.0);
        SKLufsMeter meter(TEST_SAMPLE_RATE, 1);
        meter.feed(pcm.data(), pcm.size());
        // The shelf lifts 1 kHz by ~0.7 dB; an unfiltered peak must not show it.
        ensure_approximately_equals("peak is the raw sample peak", meter.peak(), 0.5f, 8);

        SKLufsMeter quiet(TEST_SAMPLE_RATE, 1);
        std::vector<S16> zeros(TEST_SAMPLE_RATE, 0);
        quiet.feed(zeros.data(), zeros.size());
        ensure_equals("silence peaks at zero", quiet.peak(), 0.f);
    }

    // A 100 ms burst reads its energy over one 400 ms block however much
    // silence follows it, within the 3 s momentary tier.
    template<> template<>
    void sklufs_object_t::test<12>()
    {
        std::vector<S16> burst = make_sine(1000.0, 0.1, 1, 0.1);
        for (F64 seconds : { 0.1, 0.3, 1.0, 2.0, 3.0 })
        {
            std::vector<S16> clip(burst);
            clip.resize((size_t)(TEST_SAMPLE_RATE * seconds), 0);
            SKLufsMeter meter(TEST_SAMPLE_RATE, 1);
            meter.feed(clip.data(), clip.size());
            std::string what = llformat("100 ms burst padded to %.1f s", seconds);
            ensure_approximately_equals(what.c_str(), meter.close(), -23.f - 6.02f, 4);
        }
    }

    // The K-weighting at 48 kHz is BS.1770-4's published table.
    template<> template<>
    void sklufs_object_t::test<13>()
    {
        const F64 shelf_ref[5] = { 1.53512485958697, -2.69169618940638, 1.19839281085285,
                                   -1.69065929318241, 0.73248077421585 };
        const F64 highpass_ref[5] = { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };

        F64 shelf[5];
        F64 highpass[5];
        SKLufsMeter::kWeighting(48000.0, shelf, highpass);
        for (int i = 0; i < 5; ++i)
        {
            ensure(llformat("shelf coefficient %d", i), fabs(shelf[i] - shelf_ref[i]) < 1e-12);
            ensure(llformat("high pass coefficient %d", i), fabs(highpass[i] - highpass_ref[i]) < 1e-12);
        }
    }

    // EBU Tech 3341 cases 1 and 2: 20 s of stereo 1 kHz at -23 and -33 dBFS.
    template<> template<>
    void sklufs_object_t::test<14>()
    {
        for (F64 dbfs : { -23.0, -33.0 })
        {
            std::vector<S16> pcm = make_sine(1000.0, pow(10.0, dbfs / 20.0), 2, 20.0, 48000);
            SKLufsMeter meter(48000, 2);
            meter.feed(pcm.data(), pcm.size() / 2);
            const F32 lufs = meter.close();
            ensure(llformat("Tech 3341 at %.0f dBFS reads %.2f", dbfs, lufs), fabs(lufs - (F32)dbfs) <= 0.1f);
        }
    }

    // Padding a click or a hit with silence from 0.1 s to 4 s moves its reading by at most 0.5 LU.
    template<> template<>
    void sklufs_object_t::test<15>()
    {
        const F32 click = padding_spread(make_click());
        ensure(llformat("60 ms click padding spread %.2f LU", click), click <= 0.5f);

        const F32 hit = padding_spread(make_hit());
        ensure(llformat("300 ms hit padding spread %.2f LU", hit), hit <= 0.5f);
    }

    // A hit ending inside the unfinished last sub-block still counts, wherever it sits.
    template<> template<>
    void sklufs_object_t::test<16>()
    {
        const size_t total = TEST_SAMPLE_RATE * 35 / 100;
        const size_t hit_len = TEST_SAMPLE_RATE * 5 / 100;
        F32 lo = 1000.f;
        F32 hi = -1000.f;
        for (size_t offset = 0; offset + hit_len <= total; offset += TEST_SAMPLE_RATE / 100)
        {
            std::vector<F64> x(total, 0.0);
            for (size_t i = 0; i < hit_len; ++i)
            {
                x[offset + i] = 0.5 * sin(2.0 * F_PI * 1000.0 * (F64)i / TEST_SAMPLE_RATE);
            }
            const F32 l = measure_mono(x);
            lo = llmin(lo, l);
            hi = llmax(hi, l);
        }
        ensure(llformat("hit is never lost to the tail (lowest %.2f)", lo), lo > -30.f);
        ensure(llformat("tail position swing %.2f LU", hi - lo), hi - lo <= 0.5f);
    }

    // Content wholly under the -70 LUFS absolute gate reads silent in every tier.
    template<> template<>
    void sklufs_object_t::test<17>()
    {
        for (F64 seconds : { 0.2, 2.0, 6.0, 12.0 })
        {
            std::vector<S16> pcm = make_sine(1000.0, pow(10.0, -76.0 / 20.0), 1, seconds);
            SKLufsMeter meter(TEST_SAMPLE_RATE, 1);
            meter.feed(pcm.data(), pcm.size());
            ensure_equals(llformat("sub-gate %.1f s clip is silent", seconds).c_str(), meter.close(), SK_LUFS_SILENCE);
        }
    }

    // A long clip with one loud block over a quiet bed reads no lower than 8 LU under that block.
    template<> template<>
    void sklufs_object_t::test<18>()
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.1, 1, 0.4);
        std::vector<S16> bed = make_sine(1000.0, 0.008, 1, 19.6); // about -45 LUFS, inside the relative gate
        pcm.insert(pcm.end(), bed.begin(), bed.end());

        SKLufsMeter meter(TEST_SAMPLE_RATE, 1);
        meter.feed(pcm.data(), pcm.size());
        ensure_approximately_equals("integrated is floored at M - 8", meter.close(), -23.f - 8.f, 3);
    }

    // The solver scales by strength, then applies the tolerance, then the clamps, then the peak cap.
    template<> template<>
    void sklufs_object_t::test<19>()
    {
        // 0.8 LU off at half strength is 0.4 dB, inside the tolerance.
        ensure_equals("strength applies before the tolerance",
                      sk_lufs_gain_for_target(-23.8f, -23.f, 0.5f), 1.f);
        // 1.2 LU off at half strength is 0.6 dB, outside it.
        ensure_approximately_equals("just past the tolerance is corrected",
                                    sk_lufs_gain_for_target(-24.2f, -23.f, 0.5f), powf(10.f, 0.6f / 20.f), 12);
        // 40 LU at half strength is 20 dB, clamped to the boost limit.
        ensure_approximately_equals("strength applies before the clamp",
                                    sk_lufs_gain_for_target(-63.f, -23.f, 0.5f),
                                    powf(10.f, SK_LUFS_MAX_BOOST_DB / 20.f), 12);
        // The clamped 12 dB boost then meets a peak with 6 dB of headroom.
        const F32 peak = SK_LUFS_PEAK_CEILING * powf(10.f, -6.f / 20.f);
        ensure_approximately_equals("the peak cap applies after the clamp",
                                    sk_lufs_gain_for_target(-63.f, -23.f, 0.5f, peak), powf(10.f, 6.f / 20.f), 12);
        // 50 LU hot at half strength is -25 dB, clamped to the cut limit, which no peak caps.
        ensure_approximately_equals("cuts clamp and ignore the cap",
                                    sk_lufs_gain_for_target(27.f, -23.f, 0.5f, 1.f),
                                    powf(10.f, SK_LUFS_MAX_CUT_DB / 20.f), 12);
    }
}
