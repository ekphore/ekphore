// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ekphore::dsp {

struct PitchControlHistory {
    float current{};
    float previous{};
    bool initialized{};
};

struct PitchInterpolationResult {
    bool valid{};
    std::size_t controls_consumed{};
    std::uint32_t end_phase{};
};

// interpolation only. with SSE and 33+ frames the full intervals don't fuse
// but partial ones do, shorter runs always fuse. bad input changes nothing
[[nodiscard]] inline PitchInterpolationResult interpolate_dynamic_pitch(
        std::span<const float> targets, std::span<float> output,
        PitchControlHistory &history, std::uint32_t old_phase,
        bool sse_present = true) noexcept {
    const auto count = output.size() / 32 + (output.size() % 32 != 0);
    if (old_phase >= 32 || targets.size() < count)
        return { false, 0, old_phase };
    if (output.empty())
        return { true, 0, old_phase };

    if (!history.initialized) {
        history.initialized = true;
        history.current =
                targets[0]; // don't touch previous, even the first time
    }

    const bool simd_intervals = sse_present && output.size() >= 33;
    std::size_t control_index = 0;
    std::size_t written = 0;
    auto phase = old_phase;
    while (written < output.size()) {
        if (phase == 0) {
            history.previous = history.current;
            history.current = targets[control_index++];
        }
        const auto interval =
                std::min<std::size_t>(32 - phase, output.size() - written);
        const bool separate = simd_intervals && phase == 0 && interval == 32;
        const float delta = history.current - history.previous;
        float fraction = static_cast<float>(phase) * (1.0F / 32.0F);
        for (std::size_t index = 0; index < interval; ++index) {
            if (separate) {
                // round the product even with contraction on
                volatile float product = delta * fraction;
                output[written + index] = history.previous + product;
            } else {
                output[written + index] =
                        std::fma(delta, fraction, history.previous);
            }
            fraction += 1.0F / 32.0F;
        }
        written += interval;
        phase = (phase + static_cast<std::uint32_t>(interval)) & 31U;
    }
    return { true, control_index, phase };
}

} // namespace ekphore::dsp
