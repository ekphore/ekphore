// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "multi_state_variable.h"

#include <algorithm>
#include <cmath>

namespace ekphore::multi_state_variable {
namespace {

constexpr std::uint32_t first_serialized_type = 63;
constexpr std::uint32_t last_serialized_type = 65;

} // namespace

bool is_serialized_type(const std::uint32_t type) {
    return type >= first_serialized_type && type <= last_serialized_type;
}

Mode mode_from_serialized_type(const std::uint32_t type) {
    if (!is_serialized_type(type))
        return Mode::low_pass;
    return static_cast<Mode>(type - first_serialized_type);
}

float Filter::interpolated_exp2(const float position) {
    if (position < 0.0F)
        return 0.0F;
    if (position >= 2400.0F)
        return std::exp2(20.0F);
    const auto index = static_cast<std::uint32_t>(position);
    const auto table_value = [](const std::uint32_t table_index) {
        return std::exp2(static_cast<float>(table_index) / 60.0F - 20.0F);
    };
    const float lower = table_value(index);
    return lower + (table_value(index + 1U) - lower) *
                           (position - static_cast<float>(index));
}

bool Filter::configure(const std::uint32_t sample_rate, const float cutoff,
        const float resonance, const float bandwidth, const Mode mode) {
    if (sample_rate == 0)
        return false;
    if (configured_sample_rate == sample_rate && cutoff_control == cutoff &&
            resonance_control == resonance && bandwidth_control == bandwidth &&
            filter_mode == mode)
        return true;
    configured_sample_rate = sample_rate;
    cutoff_control = cutoff;
    resonance_control = resonance;
    bandwidth_control = bandwidth;
    filter_mode = mode;

    const float center_note = cutoff * 180.0F - 20.0F;
    const float width = bandwidth * 63.0F + 1.0F;
    half_width = width * 0.5F;
    const std::array<float, 2> note{ std::min(center_note - half_width, 160.0F),
        std::min(center_note + half_width, 160.0F) };
    for (std::size_t index = 0; index < 2; ++index) {
        const float frequency = std::max(
                interpolated_exp2(note[index] * 5.0F + 1381.8816F), 10.0F);
        const float tangent =
                frequency * 3.1415927F / static_cast<float>(sample_rate);
        const float tangent2 = tangent * tangent;
        section_steps[index] =
                tangent *
                (1.0F + (1.0F + tangent2 * 0.4F) * tangent2 * 0.33333334F);
        const float limited_note = std::clamp(note[index], 0.0F, 134.0F);
        section_limits[index] = 400.0F - (limited_note / 134.0F) * 376.0F;
    }
    // still normalized, cube it BEFORE the mode compensation and don't fuse
    // the multiply
    const float inverse_resonance = 1.0F - resonance;
    damping = std::fma(
            -inverse_resonance * inverse_resonance, inverse_resonance, 1.0F);
    if (mode == Mode::high_pass) {
        const float limited_half_width = std::min(half_width, 6.0F);
        damping *= (limited_half_width / 6.0F) * 0.00999999F + 0.99F;
        output_gain = std::pow(1.0F - 0.5F * std::pow(damping, 5.0F), 2.0F);
    } else {
        output_gain = (1.0F - 0.7F * std::pow(damping, 5.0F)) * 0.7F;
    }
    return true;
}

Filter::Taps Filter::process_section(const float input, State &state,
        const float step, const float limit) const {
    const float sum = step + (1.0F - damping) * 2.0F;
    const float product = step * sum;
    const float old_band = state.band;
    const float high = (input - (state.low + (step + sum) * old_band +
                                        product * state.high)) /
                       (product + 1.0F);
    const float band = old_band + step * (state.high + high);
    state.high = high;
    const float bound = limit * 2.0F;
    const float bounded_band = std::clamp(band, -bound, bound);
    state.band =
            bounded_band * (1.0F - (0.25F / limit) * std::abs(bounded_band));
    const float low = state.low + step * (old_band + band);
    const float bounded_low = std::clamp(low, -bound, bound);
    state.low = bounded_low * (1.0F - (0.25F / limit) * std::abs(bounded_low));
    return { high, band * 2.0F, low };
}

float Filter::process_channel(const float input, const std::size_t channel) {
    if (channel >= channel_states.size())
        return input;
    const auto first = process_section(input, channel_states[channel][0],
            section_steps[0], section_limits[0]);
    const float first_mix =
            filter_mode == Mode::low_pass
                    ? first.low
                    : (filter_mode == Mode::band_pass ? first.band
                                                      : first.high);
    const float second_input =
            filter_mode == Mode::high_pass ? first_mix : input;
    const auto second = process_section(second_input,
            channel_states[channel][1], section_steps[1], section_limits[1]);
    const float second_mix =
            filter_mode == Mode::low_pass
                    ? second.high
                    : (filter_mode == Mode::band_pass ? second.band
                                                      : second.low);
    if (filter_mode == Mode::high_pass)
        return output_gain * second_mix;
    float output = first_mix + second_mix;
    if (filter_mode == Mode::low_pass) {
        const float mix = (1.0F - std::min(half_width, 3.0F) / 3.0F) * 0.5F;
        output += mix * (first.band + second.band);
    }
    return output_gain * output;
}

std::pair<float, float> Filter::process(const float left, const float right) {
    return { process_channel(left, 0), process_channel(right, 1) };
}

void Filter::reset() { channel_states = {}; }

} // namespace ekphore::multi_state_variable
