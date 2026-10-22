// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "state_variable_4.h"

#include <algorithm>
#include <cmath>

namespace ekphore::state_variable_4 {
namespace {

constexpr std::uint32_t first_serialized_type = 51;
constexpr std::uint32_t last_serialized_type = 54;

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
        const float resonance, const Mode mode) {
    if (sample_rate == 0)
        return false;
    if (configured_sample_rate == sample_rate && cutoff_control == cutoff &&
            resonance_control == resonance && filter_mode == mode)
        return true;
    configured_sample_rate = sample_rate;
    cutoff_control = cutoff;
    resonance_control = resonance;
    filter_mode = mode;

    const float note = cutoff * 115.0F + 20.0F;
    const float frequency =
            std::max(interpolated_exp2(note * 5.0F + 1381.8816F), 0.0F);
    const float tangent =
            frequency * 3.1415927F / static_cast<float>(sample_rate);
    const float tangent2 = tangent * tangent;
    step = tangent * ((tangent2 * 0.4F + 1.0F) * tangent2 * 0.33333334F + 1.0F);

    filter_damping = 1.0F - std::pow(1.0F - resonance, 3.0F);
    float gain_scale = 0.77F;
    if (mode == Mode::band_pass)
        gain_scale = 0.9334F;
    else if (mode == Mode::notch)
        gain_scale = 0.88F;
    output_gain = 1.0F - gain_scale * filter_damping;

    const float limited_note = std::min(note, 134.0F);
    const float normalized = limited_note / 134.0F;
    state_limit =
            normalized * (std::abs(normalized) * -0.5F + 1.5F) * -1564.0F +
            1600.0F;
    saturation = 0.25F / state_limit;
    return true;
}

float Filter::process_section(
        const float input, SectionState &state, const float damping) const {
    const float sum = step + (1.0F - damping) * 2.0F;
    const float product = step * sum;
    const float old_band = state.band;
    const float high = (input - (state.low + (step + sum) * old_band +
                                        product * state.high)) /
                       (product + 1.0F);
    const float band = old_band + step * (state.high + high);
    state.high = high;
    const float bound = state_limit * 2.0F;
    const float limited_band = std::clamp(band, -bound, bound);
    state.band = limited_band * (1.0F - saturation * std::abs(limited_band));
    const float low = state.low + step * (old_band + band);
    const float limited_low = std::clamp(low, -bound, bound);
    state.low = limited_low * (1.0F - saturation * std::abs(limited_low));
    const float doubled_band = band + band;
    if (filter_mode == Mode::low_pass)
        return low;
    if (filter_mode == Mode::band_pass)
        return doubled_band;
    if (filter_mode == Mode::high_pass)
        return high;
    return input - doubled_band;
}

float Filter::process_channel(const float input, const std::size_t channel) {
    if (channel >= channel_states.size())
        return input;
    const float first =
            process_section(input, channel_states[channel][0], filter_damping);
    const float second_damping = filter_damping * 0.666F;
    return output_gain *
           process_section(first, channel_states[channel][1], second_damping);
}

std::pair<float, float> Filter::process(const float left, const float right) {
    return { process_channel(left, 0), process_channel(right, 1) };
}

void Filter::reset() { channel_states = {}; }

} // namespace ekphore::state_variable_4
