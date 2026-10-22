// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

namespace ekphore::legacy_limiter {

// the detector is 32 frames ahead of the audio
class Processor {
  public:
    void configure(std::uint32_t sample_rate, float input_gain_db,
            float release_time_ms) {
        if (configured_rate == sample_rate && input_level_db == input_gain_db &&
                release_ms == release_time_ms)
            return;
        configured_rate = sample_rate;
        input_level_db = input_gain_db;
        release_ms = release_time_ms;
        input_scale = std::exp2(input_gain_db * 0.16666667F);
        // clang-format off
        release_coefficient = 0.693147F / std::max(release_time_ms * 0.001F *
                static_cast<float>(sample_rate), 1.0F);
        // clang-format on

        const float x = 0.03125F * 6.28319F;
        const float x2 = x * x;
        // clang-format off
        const float sine = x * (x2 * (x2 * (x2 * (x2 * (x2 * -2.39e-08F +
                2.7526e-06F) - 0.000198409F) + 0.00833333F) - 0.166667F) +
                1.0F) * 0.91F;
        // clang-format on
        const float reciprocal = 1.0F / (sine + 1.0F);
        // clang-format off
        const float cosine = x2 * (x2 * (x2 * (x2 * (x2 * -2.605e-07F +
                2.47609e-05F) - 0.00138884F) + 0.0416666F) - 0.499923F) + 1.0F;
        // clang-format on
        coefficient_280 = -(1.0F - sine) * reciprocal;
        coefficient_278 = (cosine + cosine) * reciprocal;
        coefficient_28c = (1.0F - cosine) * 0.5F * reciprocal;
        coefficient_288 = (1.0F - cosine) * reciprocal;
        coefficient_284 = coefficient_28c;
    }

    void reset() {
        delay = {};
        delay_write = 0;
        peak_history = {};
        peak_write = 0;
        envelope_state = tiny();
        filter_state = tiny();
        filter_previous = tiny();
    }

    [[nodiscard]] std::pair<float, float> process(float left, float right) {
        const float scaled_left = left * input_scale;
        const float scaled_right = right * input_scale;
        const auto delay_index = (delay_write + 1U) & 31U;
        const std::array<float, 2> delayed{ delay[0][delay_index],
            delay[1][delay_index] };
        delay[0][delay_index] = scaled_left;
        delay[1][delay_index] = scaled_right;
        delay_write = delay_index;

        const float detector =
                std::max(std::abs(scaled_left), std::abs(scaled_right));
        const float difference = detector - envelope_state;
        float envelope = detector;
        if (difference <= 0.0F)
            envelope = envelope_state + difference * release_coefficient;
        envelope_state = envelope + tiny();

        float peak = envelope;
        for (const float history : peak_history)
            peak = std::max(peak, history);
        const auto peak_index = (peak_write + 1U) & 31U;
        peak_history[peak_index] = envelope;
        peak_write = peak_index;

        const float old_filter = filter_state;
        const float old_previous = filter_previous;
        filter_previous = old_filter + tiny();
        const float intermediate = old_previous * coefficient_280 +
                                   old_filter * coefficient_278 + peak;
        filter_state = intermediate + tiny();
        const float control = old_previous * coefficient_28c +
                              old_filter * coefficient_288 +
                              coefficient_284 * intermediate;
        const float denominator =
                control - control * (control * -0.25F + 1.0F) + 1.0F;
        const float gain = 1.0F / denominator;
        return { std::clamp(delayed[0] * gain, -1.0F, 1.0F),
            std::clamp(delayed[1] * gain, -1.0F, 1.0F) };
    }

  private:
    [[nodiscard]] static float tiny() {
        return std::bit_cast<float>(0x219392efU);
    }

    std::uint32_t configured_rate{};
    float input_level_db{};
    float release_ms{ 100.0F };
    float input_scale{ 1.0F };
    float release_coefficient{};
    std::array<std::array<float, 32>, 2> delay{};
    std::uint32_t delay_write{};
    std::array<float, 32> peak_history{};
    std::uint32_t peak_write{};
    float envelope_state{ tiny() };
    float filter_state{ tiny() };
    float filter_previous{ tiny() };
    float coefficient_278{};
    float coefficient_280{};
    float coefficient_284{};
    float coefficient_288{};
    float coefficient_28c{};
};

} // namespace ekphore::legacy_limiter
