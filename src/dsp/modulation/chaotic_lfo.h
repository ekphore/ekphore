// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "simple_lfo.h"

namespace ekphore::dsp {

// the plain chaotic LFO, NOT the multi random one
struct ChaoticLfo : SimpleLfo {
    std::uint32_t random_state{};
    float random_value{};
    double random_threshold{ .5 };

    void draw() noexcept {
        random_state = random_state * 0x0bb38435U + 0x3619636bU;
        random_value = std::bit_cast<float>(
                               (random_state & 0x007fffffU) | 0x40000000U) -
                       3.F;
    }

    bool prepare(float hz, float control_rate, float authored_start,
            std::uint32_t entropy, float delay_ms = 0.F,
            bool extended = false) noexcept {
        if (!SimpleLfo::prepare(Wave::sine, hz, control_rate, authored_start,
                    .5F, delay_ms, extended))
            return false;
        reset(entropy);
        return true;
    }

    // explicit so we can replay it
    void reset(std::uint32_t entropy) noexcept {
        SimpleLfo::reset();
        random_state = entropy;
        draw();
        random_threshold = .5;
    }

    bool generate(std::span<float> output, int remainder = 0,
            std::optional<float> start_plane = std::nullopt,
            std::span<const float> frequency_plane = {}) noexcept {
        if (remainder < 0 || remainder >= 32 ||
                (start_plane &&
                        (!std::isfinite(*start_plane) || *start_plane < 0.F ||
                                *start_plane > 1.F)))
            return false;
        if (!frequency_plane.empty()) {
            if (frequency_plane.size() != output.size())
                return false;
            for (const float value : frequency_plane) {
                if (!std::isfinite(value))
                    return false;
                const float next = mul(
                        lfo_frequency_from_unity(value), inverse_control_rate);
                if (!std::isfinite(next) || next > 1.F)
                    return false;
            }
        }
        if (reset_pending && start_plane)
            phase = *start_plane;
        reset_pending = false;
        std::size_t tick = 0;
        for (float &value : output) {
            if (!frequency_plane.empty())
                step = static_cast<double>(
                        mul(lfo_frequency_from_unity(frequency_plane[tick++]),
                                inverse_control_rate));
            value = random_value; // emit BEFORE drawing the next one
            phase += step;
            if (phase >= random_threshold) {
                draw();
                random_threshold = 1.5 - random_threshold;
            }
            if (phase > 1.)
                phase -= 1.;
        }
        const int fading =
                std::min(fade_remaining, static_cast<int>(output.size()));
        for (int i = 0; i < fading; ++i) {
            output[static_cast<std::size_t>(i)] =
                    mul(fade - .3F, output[static_cast<std::size_t>(i)]);
            fade = std::clamp(mul(fade_factor, fade), fade_extended ? .3F : 0.F,
                    fade_extended ? 1.3F : 1.F);
        }
        fade_remaining -= fading;
        phase = std::fma(
                static_cast<double>(remainder) * (-1. / 32.), step, phase);
        if (phase < 0.)
            phase += 1.;
        return true;
    }
};

// voices take the end of the shared buffer and copy phase/fade, random state
// stays their own
struct ChaoticLfoSharedTemplate {
    ChaoticLfo state;
    std::uint64_t stamp{};
    std::size_t count{};
    std::array<float, 32> values{};
    bool initialized{};

    bool consume(ChaoticLfo &voice, std::uint64_t source_stamp,
            std::span<float> output, std::span<const float> frequency,
            int remainder) noexcept {
        if (output.empty())
            return true;
        if (!source_stamp || output.size() > values.size())
            return false;
        if (stamp != source_stamp) {
            if (!voice.generate(std::span(values).first(output.size()),
                        remainder, std::nullopt, frequency))
                return false;
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
