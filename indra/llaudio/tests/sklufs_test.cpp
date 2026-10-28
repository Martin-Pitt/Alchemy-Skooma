/**
 * @file sklufs_test.cpp
 * @brief Tests for the BS.1770-4 loudness meter.
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

#include <catch2/catch_test_macros.hpp>
#include "altestmatchers.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

namespace
{
    using ll_test::WithinBits;

    constexpr U32 TEST_SAMPLE_RATE = 44100;

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

    F32 measure_pcm(const std::vector<S16>& pcm, U32 channels = 1, U32 fs = TEST_SAMPLE_RATE)
    {
        SKLufsMeter meter(fs, channels);
        meter.feed(pcm.data(), pcm.size() / channels);
        return meter.close();
    }

    F32 measure_mono(const std::vector<F64>& x)
    {
        std::vector<S16> pcm(x.size());
        for (size_t i = 0; i < x.size(); ++i)
        {
            pcm[i] = (S16)llclamp((S32)ll_round((F32)(x[i] * 32768.0)), -32768, 32767);
        }
        return measure_pcm(pcm);
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

    // A directory of the case's own, removed with everything in it when the case ends.
    struct TempDir
    {
        std::filesystem::path path;

        TempDir()
        {
            path = std::filesystem::temp_directory_path() /
                   ("sklufs_test_" + std::to_string((U64)std::hash<const void*>()(this)) + "_" +
                    std::to_string((U64)std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    // Minimal RIFF/WAVE writer, so the on-disk path can be exercised without
    // shipping a binary fixture.
    bool write_wav(const std::filesystem::path& path, const std::vector<S16>& pcm, U16 channels, U32 sample_rate)
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

    TEST_CASE("sklufs: steady sines land on the analytic prediction", "[sklufs]")
    {
        struct Case { F64 freq; F64 amp; U32 channels; };
        const Case cases[] = {
            { 1000.0, 0.10, 1 }, { 1000.0, 0.10, 2 }, { 1000.0, 1.00, 1 },
            {  100.0, 0.50, 1 }, { 4000.0, 0.25, 2 },
        };

        for (const Case& c : cases)
        {
            CAPTURE(c.freq, c.amp, c.channels);
            const F32 lufs = measure_pcm(make_sine(c.freq, c.amp, c.channels, 3.0), c.channels);
            CHECK_THAT(lufs, WithinBits((F32)expected_sine_lufs(c.freq, c.amp, c.channels), 8));
        }
    }

    TEST_CASE("sklufs: a 1 kHz sine at 0.1 peak measures -23 LUFS", "[sklufs]")
    {
        CHECK_THAT(measure_pcm(make_sine(1000.0, 0.1, 1, 3.0)), WithinBits(-23.f, 6));
    }

    TEST_CASE("sklufs: a clip under 400 ms reads its energy over one block", "[sklufs]")
    {
        const F32 lufs = measure_pcm(make_sine(1000.0, 0.1, 1, 0.2));
        CHECK(lufs > SK_LUFS_SILENCE);
        CHECK_THAT(lufs, WithinBits(-23.f - 3.01f, 4));
    }

    TEST_CASE("sklufs: chunked byte feeding agrees with one-shot feeding", "[sklufs]")
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.1, 2, 1.0);
        const F32 reference = measure_pcm(pcm, 2);

        const U8* bytes = (const U8*)pcm.data();
        const size_t byte_count = pcm.size() * sizeof(S16);
        for (size_t chunk : { (size_t)1, (size_t)3, (size_t)4095, (size_t)4096 })
        {
            CAPTURE(chunk);
            SKLufsMeter meter(TEST_SAMPLE_RATE, 2);
            for (size_t off = 0; off < byte_count; off += chunk)
            {
                meter.feedBytesLE(bytes + off, llmin(chunk, byte_count - off));
            }
            CHECK_THAT(meter.close(), WithinBits(reference, 16));
        }
    }

    TEST_CASE("sklufs: silence and empty feeds measure as silence", "[sklufs]")
    {
        std::vector<S16> quiet(TEST_SAMPLE_RATE, 0);
        CHECK(measure_pcm(quiet) == SK_LUFS_SILENCE);

        SKLufsMeter empty(TEST_SAMPLE_RATE, 1);
        empty.feed(nullptr, 0);
        empty.feedBytesLE(nullptr, 0);
        CHECK(empty.close() == SK_LUFS_SILENCE);

        SKLufsMeter bogus(0, 0);
        CHECK_FALSE(bogus.isValid());
    }

    TEST_CASE("sklufs: the solver clamps boosts and cuts", "[sklufs]")
    {
        CHECK_THAT(sk_lufs_gain_for_target(-30.f, -23.f), WithinBits(powf(10.f, 7.f / 20.f), 16));
        CHECK(sk_lufs_gain_for_target(-23.2f, -23.f) == 1.f);
        CHECK(sk_lufs_gain_for_target(SK_LUFS_SILENCE, -23.f) == 1.f);
        CHECK_THAT(sk_lufs_gain_for_target(-80.f, -23.f), WithinBits(powf(10.f, SK_LUFS_MAX_BOOST_DB / 20.f), 16));
        CHECK_THAT(sk_lufs_gain_for_target(0.f, -60.f), WithinBits(powf(10.f, SK_LUFS_MAX_CUT_DB / 20.f), 16));
    }

    TEST_CASE("sklufs: applying the solved gain re-measures on target", "[sklufs]")
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.1, 1, 3.0);
        const F32 gain = sk_lufs_gain_for_target(measure_pcm(pcm), -18.f);
        for (S16& v : pcm)
        {
            v = (S16)llclamp((S32)ll_round((F32)v * gain), -32768, 32767);
        }
        CHECK_THAT(measure_pcm(pcm), WithinBits(-18.f, 6));
    }

    TEST_CASE("sklufs: a WAV file on disk measures as its header describes it", "[sklufs]")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "tone.wav";

        REQUIRE(write_wav(path, make_sine(1000.0, 0.1, 1, 1.0), 1, TEST_SAMPLE_RATE));
        F32 lufs = SK_LUFS_SILENCE;
        F32 peak = 0.f;
        CHECK(sk_lufs_measure_wav_file(path.string(), lufs, peak));
        CHECK_THAT(lufs, WithinBits(-23.f, 6));

        // Identical channels: as stereo the powers sum (+3 LU).
        REQUIRE(write_wav(path, make_sine(1000.0, 0.1, 2, 1.0), 2, TEST_SAMPLE_RATE));
        F32 as_stereo = SK_LUFS_SILENCE;
        CHECK(sk_lufs_measure_wav_file(path.string(), as_stereo, peak));
        CHECK_THAT(as_stereo, WithinBits(-20.f, 6));

        F32 unused = SK_LUFS_SILENCE;
        CHECK_FALSE(sk_lufs_measure_wav_file((dir.path / "does_not_exist.wav").string(), unused, peak));
    }

    TEST_CASE("sklufs: strength scales the correction in dB", "[sklufs]")
    {
        CHECK_THAT(sk_lufs_gain_for_target(-33.f, -23.f, 1.f), WithinBits(powf(10.f, 10.f / 20.f), 16));
        CHECK_THAT(sk_lufs_gain_for_target(-33.f, -23.f, 0.5f), WithinBits(powf(10.f, 5.f / 20.f), 16));
        CHECK(sk_lufs_gain_for_target(-40.f, -23.f, 0.f) == 1.f);

        // Two clips 10 LU apart stay 5 LU apart at half strength.
        const F32 quiet = sk_lufs_gain_for_target(-33.f, -23.f, 0.5f);
        const F32 loud  = sk_lufs_gain_for_target(-23.f, -23.f, 0.5f);
        CHECK_THAT(10.f - 20.f * log10f(quiet / loud), WithinBits(5.f, 5));
    }

    TEST_CASE("sklufs: a boost is capped by the sample peak and a cut is not", "[sklufs]")
    {
        CHECK(sk_lufs_gain_for_target(-40.f, -23.f, 1.f, SK_LUFS_PEAK_CEILING) == 1.f);
        CHECK(sk_lufs_gain_for_target(-40.f, -23.f, 1.f, 1.f) == 1.f);
        CHECK_THAT(0.5f * sk_lufs_gain_for_target(-40.f, -23.f, 1.f, 0.5f), WithinBits(SK_LUFS_PEAK_CEILING, 16));
        CHECK_THAT(sk_lufs_gain_for_target(-30.f, -23.f, 1.f, 0.01f), WithinBits(powf(10.f, 7.f / 20.f), 16));
        CHECK_THAT(sk_lufs_gain_for_target(-10.f, -23.f, 1.f, 1.f), WithinBits(powf(10.f, -13.f / 20.f), 16));
        CHECK_THAT(sk_lufs_gain_for_target(-30.f, -23.f, 1.f, 0.f), WithinBits(powf(10.f, 7.f / 20.f), 16));
    }

    TEST_CASE("sklufs: the peak is the raw sample peak before K-weighting", "[sklufs]")
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.5, 1, 1.0);
        SKLufsMeter meter(TEST_SAMPLE_RATE, 1);
        meter.feed(pcm.data(), pcm.size());
        CHECK_THAT(meter.peak(), WithinBits(0.5f, 8));

        SKLufsMeter quiet(TEST_SAMPLE_RATE, 1);
        std::vector<S16> zeros(TEST_SAMPLE_RATE, 0);
        quiet.feed(zeros.data(), zeros.size());
        CHECK(quiet.peak() == 0.f);
    }

    TEST_CASE("sklufs: a short burst reads the same however much silence follows", "[sklufs]")
    {
        std::vector<S16> burst = make_sine(1000.0, 0.1, 1, 0.1);
        for (F64 seconds : { 0.1, 0.3, 1.0, 2.0, 3.0 })
        {
            CAPTURE(seconds);
            std::vector<S16> clip(burst);
            clip.resize((size_t)(TEST_SAMPLE_RATE * seconds), 0);
            CHECK_THAT(measure_pcm(clip), WithinBits(-23.f - 6.02f, 4));
        }
    }

    TEST_CASE("sklufs: the K-weighting at 48 kHz is the published table", "[sklufs]")
    {
        const F64 shelf_ref[5] = { 1.53512485958697, -2.69169618940638, 1.19839281085285,
                                   -1.69065929318241, 0.73248077421585 };
        const F64 highpass_ref[5] = { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };

        F64 shelf[5];
        F64 highpass[5];
        SKLufsMeter::kWeighting(48000.0, shelf, highpass);
        for (int i = 0; i < 5; ++i)
        {
            CAPTURE(i, shelf[i], highpass[i]);
            CHECK(fabs(shelf[i] - shelf_ref[i]) < 1e-12);
            CHECK(fabs(highpass[i] - highpass_ref[i]) < 1e-12);
        }
    }

    TEST_CASE("sklufs: EBU Tech 3341 cases 1 and 2", "[sklufs]")
    {
        for (F64 dbfs : { -23.0, -33.0 })
        {
            CAPTURE(dbfs);
            const F32 lufs = measure_pcm(make_sine(1000.0, pow(10.0, dbfs / 20.0), 2, 20.0, 48000), 2, 48000);
            CAPTURE(lufs);
            CHECK(fabs(lufs - (F32)dbfs) <= 0.1f);
        }
    }

    TEST_CASE("sklufs: padding a click or a hit from 0.1 to 4 s moves it at most 0.5 LU", "[sklufs]")
    {
        const F32 click = padding_spread(make_click());
        CAPTURE(click);
        CHECK(click <= 0.5f);

        const F32 hit = padding_spread(make_hit());
        CAPTURE(hit);
        CHECK(hit <= 0.5f);
    }

    TEST_CASE("sklufs: a hit in the unfinished last sub-block still counts", "[sklufs]")
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
        CAPTURE(lo, hi);
        CHECK(lo > -30.f);
        CHECK(hi - lo <= 0.5f);
    }

    TEST_CASE("sklufs: content under the absolute gate reads silent in every tier", "[sklufs]")
    {
        for (F64 seconds : { 0.2, 2.0, 6.0, 12.0 })
        {
            CAPTURE(seconds);
            CHECK(measure_pcm(make_sine(1000.0, pow(10.0, -76.0 / 20.0), 1, seconds)) == SK_LUFS_SILENCE);
        }
    }

    TEST_CASE("sklufs: a long clip reads no lower than 8 LU under its loudest block", "[sklufs]")
    {
        std::vector<S16> pcm = make_sine(1000.0, 0.1, 1, 0.4);
        std::vector<S16> bed = make_sine(1000.0, 0.008, 1, 19.6); // about -45 LUFS, inside the relative gate
        pcm.insert(pcm.end(), bed.begin(), bed.end());
        CHECK_THAT(measure_pcm(pcm), WithinBits(-23.f - 8.f, 3));
    }

    TEST_CASE("sklufs: the solver applies strength then tolerance then clamp then cap", "[sklufs]")
    {
        // 0.8 LU off at half strength is 0.4 dB, inside the tolerance.
        CHECK(sk_lufs_gain_for_target(-23.8f, -23.f, 0.5f) == 1.f);
        // 1.2 LU off at half strength is 0.6 dB, outside it.
        CHECK_THAT(sk_lufs_gain_for_target(-24.2f, -23.f, 0.5f), WithinBits(powf(10.f, 0.6f / 20.f), 12));
        // 40 LU at half strength is 20 dB, clamped to the boost limit.
        CHECK_THAT(sk_lufs_gain_for_target(-63.f, -23.f, 0.5f), WithinBits(powf(10.f, SK_LUFS_MAX_BOOST_DB / 20.f), 12));
        // The clamped 12 dB boost then meets a peak with 6 dB of headroom.
        const F32 peak = SK_LUFS_PEAK_CEILING * powf(10.f, -6.f / 20.f);
        CHECK_THAT(sk_lufs_gain_for_target(-63.f, -23.f, 0.5f, peak), WithinBits(powf(10.f, 6.f / 20.f), 12));
        // 50 LU hot at half strength is -25 dB, clamped to the cut limit, which no peak caps.
        CHECK_THAT(sk_lufs_gain_for_target(27.f, -23.f, 0.5f, 1.f), WithinBits(powf(10.f, SK_LUFS_MAX_CUT_DB / 20.f), 12));
    }
}
