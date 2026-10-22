// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "multi_lfo.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace ekphore::dsp {

// free running Hz, NOT synced
inline float lfo_frequency_from_unity(float unity) noexcept {
    const auto multiply = [](float a, float b) {
        volatile float product = a * b;
        return product;
    };
    const auto add = [](float a, float b) {
        volatile float sum = a + b;
        return sum;
    };
    const float x = std::clamp(unity, 0.F, 1.F);
    float value = std::fma(x, std::bit_cast<float>(0x411ec56eU),
            std::bit_cast<float>(0xc0920b78U));
    value = multiply(value, std::bit_cast<float>(0x3fb8aa3bU));
    const float fraction = value - std::floor(value);
    const float curve = std::fma(-fraction, fraction, fraction);
    const float correction = multiply(curve, std::bit_cast<float>(0xbeadee78U));
    value = add(value, std::bit_cast<float>(0x42fe0000U));
    value = add(value, correction);
    const auto bits = static_cast<std::int32_t>(multiply(value, 8388608.F));
    return std::bit_cast<float>(bits);
}

inline float lfo_frequency_from_script(std::int32_t raw) noexcept {
    // scale in double first
    return lfo_frequency_from_unity(
            static_cast<float>(static_cast<double>(raw) *
                               std::bit_cast<double>(0x3eb0c6f7a0b5ed8dULL)));
}

// NOT the inverse of frequency_step
inline float lfo_frequency_default(float hz) noexcept {
    const auto multiply = [](float a, float b) {
        volatile float product = a * b;
        return product;
    };
    const auto add = [](float a, float b) {
        volatile float sum = a + b;
        return sum;
    };
    float value = static_cast<float>(std::bit_cast<std::int32_t>(hz));
    value = multiply(value, std::bit_cast<float>(0x34000000U));
    value = add(value, std::bit_cast<float>(0xc2fe0000U));
    const float fraction = value - std::floor(value);
    const float curve = std::fma(-fraction, fraction, fraction);
    value = add(value, multiply(curve, std::bit_cast<float>(0x3eb17679U)));
    value = multiply(value, std::bit_cast<float>(0x3f317218U));
    value = add(value, std::bit_cast<float>(0x40922ab7U));
    value /= std::bit_cast<float>(0x411ef2f6U);
    return std::clamp(value, 0.F, 1.F);
}

// one waveform, only sine and triangle take a frequency plane
struct SimpleLfo {
    enum class Wave { sine, rectangle, saw, triangle };
    Wave wave{ Wave::sine };
    double phase{}, step{}, start_phase{};
    float inverse_control_rate{}; // for the frequency plane
    float pulse_width{ .5F }, fade{ .3F }, fade_factor{ 1.F };
    int fade_remaining{}, initial_fade_ticks{};
    bool fade_extended{}, reset_pending{ true };

    static float mul(float a, float b) noexcept {
        volatile float result = a * b;
        return result;
    }

    // keep the float order, only the explicit fma is fused
    static float lookup(const std::array<std::uint32_t, 4096> &table,
            double position) noexcept;

    // deadband in float, then two lookups and fix the bias in double
    static float rectangle(double position, float width) noexcept;

    // Hz and ms, NOT normalized. the multiply is float before the step becomes
    // a double
    bool prepare(Wave shape, float hz, float control_rate, float authored_start,
            float width = .5F, float delay_ms = 0.F,
            bool extended = false) noexcept;

    // back to the start
    void reset() noexcept {
        phase = start_phase;
        fade_remaining = initial_fade_ticks;
        fade = std::bit_cast<float>(0x3e99999aU);
        reset_pending = true;
    }

    // these three only
    void copy_state(const SimpleLfo &other) noexcept {
        fade = other.fade;
        fade_remaining = other.fade_remaining;
        phase = other.phase;
    }

    // NOT for clones
    void advance(int control_ticks) noexcept {
        phase = std::fmod(
                std::fma(step, static_cast<double>(control_ticks), phase), 1.);
    }

    // the plane has to match output, rejects anything we can't do BEFORE
    // touching anything
    bool generate(std::span<float> output, int fractional_audio_frames = 0,
            std::optional<float> start_plane = std::nullopt,
            std::span<const float> frequency_plane = {}) noexcept;
};

// once per control on the source BEFORE intensity, NOT per sample. only the
// positive case for now
struct SimpleLfoPositiveSmoothing {
    float coefficient{};
    float value{ -1e6F }; // sentinel until the first note

    // keeps the last output
    bool prepare(std::int16_t milliseconds, float audio_rate) noexcept {
        if (milliseconds <= 0 || !std::isfinite(audio_rate) ||
                audio_rate <= 0.F)
            return false;
        const double ticks = (static_cast<double>(milliseconds) / 1000.) *
                             static_cast<double>(audio_rate) * .03125;
        coefficient =
                static_cast<float>(1. - std::exp(-4.605170185988092 / ticks));
        return true;
    }

    void reset() noexcept { value = -1e6F; }

    float process(float transformed_source) noexcept {
        if (value < -1e5F) {
            value = transformed_source;
        } else {
            const float delta = transformed_source - value;
            if (!(std::abs(delta) < 1e-5F)) {
                // multiply then add, NOT fused
                volatile float movement = delta * coefficient;
                value = value + movement;
            }
        }
        return value;
    }
};

// one per template, it outlives the voices
struct SimpleLfoSharedTemplate {
    SimpleLfo state;
    std::uint64_t stamp{}; // starts at 0, the first real stamp is 1
    std::size_t count{};
    std::array<float, 32> values{};

    bool consume(SimpleLfo &voice, std::uint64_t source_stamp,
            std::span<float> output, int remainder) noexcept {
        if (output.empty())
            return true;
        if (source_stamp == 0 || output.size() > values.size())
            return false;
        if (stamp != source_stamp) {
            voice.generate(std::span(values).first(output.size()), remainder);
            state.copy_state(voice);
            count = output.size();
            stamp = source_stamp;
        }
        // asking for more than we have would read garbage, refuse
        if (output.size() > count)
            return false;
        std::copy_n(values.begin() + (count - output.size()), output.size(),
                output.begin());
        voice.copy_state(state);
        return true;
    }
};

// retriggered voices reuse the start for the same stamp, no suffix offset
struct SimpleLfoLocalCache {
    std::uint64_t stamp{};
    std::size_t count{};
    std::array<float, 32> values{};

    bool consume(SimpleLfo &voice, std::uint64_t source_stamp,
            std::span<float> output, int remainder,
            std::span<const float> frequency_plane = {}) noexcept {
        if (output.empty())
            return true;
        if (source_stamp == 0 || output.size() > values.size())
            return false;
        if (stamp != source_stamp) {
            if (!voice.generate(std::span(values).first(output.size()),
                        remainder, std::nullopt, frequency_plane))
                return false;
            count = output.size();
            stamp = source_stamp;
        }
        // same, don't read garbage
        if (output.size() > count)
            return false;
        std::copy_n(values.begin(), output.size(), output.begin());
        return true;
    }
};

} // namespace ekphore::dsp
