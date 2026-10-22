// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "state_variable_1.h"

#include <algorithm>
#include <cmath>

namespace ekphore::state_variable_1 {
namespace {

constexpr std::uint32_t low_pass_type = 42;
constexpr std::uint32_t high_pass_type = 43;

} // namespace

bool is_serialized_type(const std::uint32_t type) {
    return type == low_pass_type || type == high_pass_type;
}

Mode mode_from_serialized_type(const std::uint32_t type) {
    return type == high_pass_type ? Mode::high_pass : Mode::low_pass;
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

bool Filter::configure(
        const std::uint32_t sample_rate, const float cutoff, const Mode mode) {
    if (sample_rate == 0)
        return false;
    if (configured_sample_rate == sample_rate && cutoff_control == cutoff &&
            filter_mode == mode)
        return true;
    configured_sample_rate = sample_rate;
    cutoff_control = cutoff;
    filter_mode = mode;

    const float note = std::min(cutoff * 115.0F + 20.0F, 160.0F);
    const float frequency =
            std::max(interpolated_exp2(note * 5.0F + 1381.8816F), 0.0F);
    const float tangent =
            frequency * 3.1415927F / static_cast<float>(sample_rate);
    const float tangent2 = tangent * tangent;
    coefficient = tangent *
                  ((tangent2 * 0.4F + 1.0F) * tangent2 * 0.33333334F + 1.0F);
    return true;
}

float Filter::process_channel(const float input, const std::size_t channel) {
    if (channel >= channel_states.size())
        return input;
    auto &state = channel_states[channel];
    const float low =
            (state + state + input * coefficient) / (coefficient + 1.0F);
    const float high = input - low;
    state += high * coefficient;
    return filter_mode == Mode::low_pass ? low : high;
}

std::pair<float, float> Filter::process(const float left, const float right) {
    return { process_channel(left, 0), process_channel(right, 1) };
}

void Filter::reset() { channel_states.fill(0.0F); }

} // namespace ekphore::state_variable_1
