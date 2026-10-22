// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "daft.h"

#include <algorithm>
#include <cmath>

namespace ekphore::filter_daft {

bool is_serialized_type(const std::uint32_t type) {
    return type == 68 || type == 69;
}

Mode mode_from_serialized_type(const std::uint32_t type) {
    return type == 69 ? Mode::high_pass : Mode::low_pass;
}

float Filter::exp_table(const float position) {
    if (position < 0.0F)
        return 0.0F;
    if (position >= 2400.0F)
        return std::exp2(20.0F);
    const auto index = static_cast<std::uint32_t>(position);
    const auto entry = [](const std::uint32_t value) {
        return std::exp2(static_cast<float>(value) / 60.0F - 20.0F);
    };
    const float first = entry(index);
    return first +
           (entry(index + 1U) - first) * (position - static_cast<float>(index));
}

bool Filter::configure(const std::uint32_t sample_rate, const float input_gain,
        const float cutoff, const float resonance, const Mode mode) {
    if (sample_rate == 0)
        return false;
    if (configured_sample_rate == sample_rate &&
            input_gain_control == input_gain && cutoff_control == cutoff &&
            resonance_control == resonance && filter_mode == mode)
        return true;
    const bool rate_changed = configured_sample_rate != sample_rate;
    configured_sample_rate = sample_rate;
    input_gain_control = input_gain;
    cutoff_control = cutoff;
    resonance_control = resonance;
    filter_mode = mode;

    input_scale = exp_table((input_gain * 12.0F) * 9.965784F + 1200.0F);
    cutoff_hz =
            std::clamp(exp_table((cutoff * 125.0F + 20.0F) * 5.0F + 1381.8816F),
                    1.0F, 30000.0F);
    frequency_correction =
            (std::min(cutoff_hz * 0.5F, 14000.0F) / 14000.0F) * -0.8F + 1.6F;
    resonance_coefficient = 1.0F - (1.0F - resonance) * (1.0F - resonance);
    low_weight = mode == Mode::low_pass ? 1.0F : 0.0F;
    high_weight = mode == Mode::high_pass ? 1.0F : 0.0F;
    if (rate_changed)
        reset();
    return true;
}

float Filter::process_oversampled(const float input, State &state) const {
    const float driven = input * input_scale * 0.6F;
    const float internal_rate =
            static_cast<float>(configured_sample_rate) * 2.0F;
    const float x = cutoff_hz * 3.1415927F / internal_rate;
    const float x2 = x * x;
    const float tangent = x * ((x2 * 0.4F + 1.0F) * x2 * 0.33333334F + 1.0F);
    const float half_x = cutoff_hz * 0.5F * 3.1415927F / internal_rate;
    const float half_x2 = half_x * half_x;
    const float half_tangent =
            half_x * ((half_x2 * 0.4F + 1.0F) * half_x2 * 0.33333334F + 1.0F);
    const float feedback = resonance_coefficient * 0.8F;
    const float damping = (1.0F - feedback) * 2.0F;
    const float doubled_second = state.second * 2.0F;
    const float coupled = doubled_second * tangent + state.first * 2.0F;
    const float denominator_term = tangent * tangent + damping * tangent;
    const float combined = coupled + damping * doubled_second;
    const float inverse = 1.0F / (denominator_term + 1.0F);
    const float squared = tangent * tangent * inverse;
    const float auxiliary_inverse = 1.0F / (tangent + 1.0F);
    // clang-format off
    float nonlinear = feedback * (((coupled - squared * combined) + squared *
            (driven - state.auxiliary * 2.0F * auxiliary_inverse)) / (1.0F -
            feedback * auxiliary_inverse * squared));
    // clang-format on
    nonlinear -= auxiliary_inverse *
                 (state.auxiliary * 2.0F + half_tangent * nonlinear);
    state.auxiliary += nonlinear * half_tangent;
    nonlinear = std::clamp(nonlinear, -1.0F, 1.0F);
    float shaped = driven + frequency_correction * nonlinear *
                                    (1.0F - std::abs(nonlinear) * 0.5F);
    shaped = std::clamp(shaped, -4.0F, 4.0F);
    shaped *= 1.0F - std::abs(shaped) * 0.125F;
    const float old_second = state.second;
    const float old_first = state.first;
    const float next_second =
            old_second +
            tangent *
                    (shaped - inverse * (combined + denominator_term * shaped));
    state.second = next_second;
    const float next_first = old_first + tangent * (old_second + next_second);
    state.first = next_first;
    // the trapezoidal output needs old + next, not the updated state alone
    const float low = old_first + next_first;
    return (1.8F - resonance_coefficient * resonance_coefficient * 1.1999999F) *
           (low_weight * low + (shaped - low) * high_weight);
}

float Filter::process_channel(const float input, const std::size_t channel) {
    if (channel >= channel_states.size() || configured_sample_rate == 0)
        return input;
    auto &state = channel_states[channel];
    const auto doubled = state.converter.upsample(input);
    return state.converter.downsample(process_oversampled(doubled.first, state),
            process_oversampled(doubled.second, state));
}

std::pair<float, float> Filter::process(const float left, const float right) {
    return { process_channel(left, 0), process_channel(right, 1) };
}

void Filter::reset() {
    for (auto &state : channel_states) {
        state.first = 0.0F;
        state.second = 0.0F;
        state.auxiliary = 0.0F;
        state.converter.reset();
    }
}

} // namespace ekphore::filter_daft
