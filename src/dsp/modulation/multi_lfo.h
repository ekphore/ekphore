// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace ekphore::dsp::lfo_rom {
extern const std::array<std::uint32_t, 4096> sine;
extern const std::array<std::uint32_t, 4096> saw;
extern const std::array<std::uint32_t, 4096> triangle;
extern const std::array<std::uint32_t, 4096> rectangle;
} // namespace ekphore::dsp::lfo_rom

#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>

namespace ekphore::dsp {

// phase and step are doubles but keep the float roundings
struct MultiLfo {
    double phase{}, step{}, random_threshold{ .5 };
    float pulse_width{ .5F }, random_value{}, fade{ .3F }, fade_factor{ 1.F };
    std::uint32_t random_state{};
    int fade_remaining{};
    bool normalize{}, fade_extended{};
    // NOT the same order as in the file
    std::array<float, 5> weights{}; // sine, saw, triangle, rectangle, random

    // phase and fade only, NOT a full copy
    void copy_state(const MultiLfo &source) noexcept {
        fade = source.fade;
        fade_remaining = source.fade_remaining;
        phase = source.phase;
    }

    static float mul(float x, float y) noexcept {
        volatile float result = x * y;
        return result;
    }

    static float lookup(const std::array<std::uint32_t, 4096> &table,
            double position) noexcept;

    // target26 is normalized, not Hz, and the step rounds to float first
    static double frequency_step(
            float control, float inverse_control_rate) noexcept {
        control = std::clamp(control, 0.F, 1.F);
        float exponent = std::fma(control, std::bit_cast<float>(0x411ec56eU),
                std::bit_cast<float>(0xc0920b78U));
        exponent = mul(exponent, std::bit_cast<float>(0x3fb8aa3bU));
        const float fraction = exponent - std::floor(exponent);
        const float curve = std::fma(-fraction, fraction, fraction);
        const float correction = mul(curve, std::bit_cast<float>(0xbeadee78U));
        // these adds round separately; don't fuse them into the approximation
        exponent = exponent + std::bit_cast<float>(0x42fe0000U);
        exponent = exponent + correction;
        const auto bits = static_cast<std::int32_t>(mul(exponent, 8388608.F));
        const float hz = std::bit_cast<float>(bits);
        return static_cast<double>(mul(inverse_control_rate, hz));
    }

    void generate(std::span<float> output, int fractional_frames = 0) noexcept {
        generate_impl(output, {}, 0.F, fractional_frames);
    }

    // empty keeps the static step, otherwise one target26 per tick
    void generate(std::span<float> output,
            std::span<const float> frequency_controls,
            float inverse_control_rate, int fractional_frames = 0) noexcept {
        assert(frequency_controls.empty() ||
                frequency_controls.size() == output.size());
        generate_impl(output, frequency_controls, inverse_control_rate,
                fractional_frames);
    }

  private:
    void generate_impl(std::span<float> output,
            std::span<const float> frequency_controls,
            float inverse_control_rate, int fractional_frames) noexcept;
};

// the first voice generates, the others take the end of it and copy
// phase/fade. idle time doesn't move the clock
struct MultiLfoSharedTemplate {
    MultiLfo state;
    std::uint64_t stamp{};
    std::size_t count{};
    std::array<float, 32> values{};

    bool consume(MultiLfo &voice, std::uint64_t source_stamp,
            std::span<float> output, std::span<const float> frequency_controls,
            float inverse_control_rate, int remainder) noexcept {
        if (output.empty())
            return true;
        if (!source_stamp || output.size() > values.size() ||
                (!frequency_controls.empty() &&
                        frequency_controls.size() != output.size()))
            return false;
        if (stamp != source_stamp) {
            voice.generate(std::span(values).first(output.size()),
                    frequency_controls, inverse_control_rate, remainder);
            state.copy_state(voice);
            count = output.size();
            stamp = source_stamp;
        }
        if (output.size() > count)
            return false;
        std::copy_n(values.begin() + (count - output.size()), output.size(),
                output.begin());
        voice.copy_state(state);
        return true;
    }
};

} // namespace ekphore::dsp
